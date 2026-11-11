/*
 * PSn00bSDK CD-ROM library (common functions)
 * (C) 2020-2022 Lameguy64, spicyjpeg - MPL licensed
 */

#include <stdint.h>
#include <assert.h>
#include <psxetc.h>
#include <psxapi.h>
#include <psxgpu.h>
#include <psxcd.h>
#include <hwregs_c.h>

#define CD_ACK_TIMEOUT		0x100000
#define CD_SYNC_TIMEOUT		0x100000
#define MAX_RESULT_SIZE		32
#define CD_QUEUE_CAPACITY	8
#define CD_QUEUE_TIMEOUT		300

/* Internal globals */

static CdlCB _ready_callback = (CdlCB) 0;
static CdlCB _sync_callback  = (CdlCB) 0;
static CdlCB _pause_callback = (CdlCB) 0;
static CdlCB _ack_callback   = (CdlCB) 0;
static uint8_t _ack_result[MAX_RESULT_SIZE];

static uint8_t *_result_ptr;
static uint8_t _last_command, _last_mode;
static CdlLOC  _last_pos;

static volatile uint8_t _last_status, _last_irq, _last_error;
static volatile uint8_t _ack_pending, _sync_pending;

typedef struct {
	CdlQueueCB callback;
	uint8_t command;
	uint8_t parameters[3];
	uint8_t has_parameters;
} QueuedCommand;

static QueuedCommand _command_queue[CD_QUEUE_CAPACITY];
static uint8_t _queue_head;
static uint8_t _queue_length;
static uint8_t _queue_stalled;
static volatile uint8_t _queue_active;
static volatile CdlIntrResult _queue_result;
static uint8_t _queue_response[MAX_RESULT_SIZE];
static uint32_t _queue_start_frame;

// These globals are accessed by other parts of the library.
CdlCB _cd_override_callback;
volatile int _cd_media_changed;

/* Command metadata */

// The original implementation of CdControlF() and the IRQ handler used a
// ridiculous number of branches to account for command-specific behavior.
// Storing that information as an array of bitfields is far more efficient.
typedef enum {
	PARAM_1		= 1,
	PARAM_2		= 2,
	PARAM_3		= 3,
	STATUS		= 1 << 2,	// First byte of CdlAcknowledge response is status
	C_STATUS	= 1 << 3,	// First byte of CdlComplete response is status
	BLOCKING	= 1 << 4,	// Command triggers CdlComplete interrupt
	OPTIONAL	= 1 << 5,	// Parameter is optional for the command
	SETLOC		= 1 << 6	// Parameter shall be sent as a separate CdlSetloc
} CommandFlags;

// https://problemkaputt.de/psx-spx.htm#cdromcontrollercommandsummary
static const uint8_t _command_flags[] = {
	0,
	STATUS,									// CdlNop
	STATUS | PARAM_3,						// CdlSetloc
	STATUS | OPTIONAL | PARAM_1,			// CdlPlay
	//STATUS | SETLOC,						// CdlPlay
	STATUS,									// CdlForward
	STATUS,									// CdlBackward
	STATUS | SETLOC,						// CdlReadN
	STATUS | C_STATUS | BLOCKING,			// CdlStandby
	STATUS | C_STATUS | BLOCKING,			// CdlStop
	STATUS | C_STATUS | BLOCKING,			// CdlPause
	STATUS | C_STATUS | BLOCKING,			// CdlInit
	STATUS,									// CdlMute
	STATUS,									// CdlDemute
	STATUS | PARAM_2,						// CdlSetfilter
	STATUS | PARAM_1,						// CdlSetmode
	STATUS,									// CdlGetparam
	0,										// CdlGetlocL
	0,										// CdlGetlocP
	STATUS | C_STATUS | BLOCKING | PARAM_1,	// CdlSetsession
	STATUS,									// CdlGetTN
	STATUS | PARAM_1,						// CdlGetTD
	STATUS | C_STATUS | BLOCKING | SETLOC,	// CdlSeekL
	STATUS | C_STATUS | BLOCKING | SETLOC,	// CdlSeekP
	0,
	0,
	PARAM_1,								// CdlTest
	STATUS | C_STATUS | BLOCKING,			// CdlGetID
	STATUS | SETLOC,						// CdlReadS
	STATUS,									// CdlReset
	STATUS | BLOCKING | PARAM_2,			// CdlGetQ
	STATUS | C_STATUS | BLOCKING			// CdlReadTOC
};

/* Private interrupt handler */

static void _update_status(uint8_t status) {
	uint8_t last = _last_status;
	_last_status = status;

	if (!(last & CdlStatError) && (status & CdlStatError))
		_sdk_log("drive error, status=0x%02x, code=0x%02x\n", _last_status, _last_error);

	if (!(last & CdlStatShellOpen) && (status & CdlStatShellOpen)) {
		_sdk_log("shell opened, invalidating cache\n");
		_cd_media_changed = 1;
	}
}

static void _cd_irq_handler(void) {
	CD_REG(0) = 1;
	CdlIntrResult irq = CD_REG(3) & 7;

	if (irq == CdlDataReady) {
		// TODO: are the first 4 accesses really needed, or was this just
		// Sony's (dumb) way to flush the KUSEG write queue? We definitely
		// don't need to do that since we're using KSEG1.
		CD_REG(0) = 0;
		CD_REG(0);
		CD_REG(3) = 0;
		CD_REG(3);
		CD_REG(0) = 0;
		CD_REG(3) = 0x80; // Request data
	}

	CD_REG(0) = 1;
	CD_REG(3) = 0x1f; // Acknowledge all IRQs
	CD_REG(3) = 0x40; // Reset parameter buffer

	//while (CD_REG(0) & (1 << 5))
		//CD_REG(1);
	for (int i = 0; i < 50; i++)
		__asm__ volatile("");

	if (!irq || (irq > CdlDiskError))
		return;

	// Fetch the result from the drive if a buffer was previously set up. The
	// first byte is always read as it contains the drive status for most
	// commands.
	uint8_t first_byte = CD_REG(1);
	CdlCB   callback;

	CdlCB ack_callback = (CdlCB) 0;
	if ((irq == CdlAcknowledge) || ((irq == CdlDiskError) && _ack_pending)) {
		ack_callback = _ack_callback;
	}
	uint8_t *result = _result_ptr;
	if (((irq == CdlDataReady) || (irq == CdlDataEnd)) && _ack_pending) {
		// A sector/report IRQ must not overwrite the pending command's result.
		result = (uint8_t *) 0;
	} else if (ack_callback && !result) {
		result = _ack_result;
	}
	if (result) {
		result[0] = first_byte;

		for (int i = 1; (CD_REG(0) & 0x20) && (i < MAX_RESULT_SIZE); i++) {
			result[i] = CD_REG(1);
		}
	}

	switch (irq) {
		case CdlDataReady:
			// CdRead() can override any callback set using CdReadyCallback()
			// by setting _cd_override_callback.
			callback = _cd_override_callback;
			if (!callback)
				callback = _ready_callback;

			_update_status(first_byte);
			break;

		case CdlComplete:
			_sync_pending = 0;
			callback      = _sync_callback;

			if (_last_command <= CdlReadTOC) {
				if (_command_flags[_last_command] & C_STATUS)
					_update_status(first_byte);
			}
			break;

		case CdlAcknowledge:
			_ack_pending = 0;
			callback     = (CdlCB) 0;

			if (_last_command <= CdlReadTOC) {
				if (_command_flags[_last_command] & STATUS)
					_update_status(first_byte);
			}
			break;

		case CdlDataEnd:
			callback = _pause_callback;

			_update_status(first_byte);
			break;

		case CdlDiskError:
			_last_error = result ? result[1] : CD_REG(1);
			callback    = _ready_callback;

			if (_ack_pending || _sync_pending) {
				if (_sync_callback)
					_sync_callback(irq, _result_ptr);

				_ack_pending  = 0;
				_sync_pending = 0;
			}

			_update_status(first_byte);
			break;
	}

	if (callback) {
		callback(irq, result);
	}

	_last_irq = irq;
	if ((irq != CdlDataReady) && (irq != CdlDataEnd)) {
		_last_command = 0;
		_result_ptr = (uint8_t *) 0;
	}
	if (ack_callback) {
		// Finish this request before allowing the callback to issue another.
		ack_callback(irq, result);
	}
	if (_queue_active && ((irq == CdlAcknowledge) || (irq == CdlComplete) || (irq == CdlDiskError))) {
		_queue_result = irq;
	}
}

/* Initialization */

int CdInit(void) {
	int _exit = EnterCriticalSection();

	InterruptCallback(IRQ_CD, &_cd_irq_handler);
	if (_exit)
		ExitCriticalSection();

	BUS_CD_CFG = 0x00020943;

	SetDMAPriority(DMA_CD, 3);
	DMA_CHCR(DMA_CD) = 0x00000000; // Stop DMA

	CD_REG(0) = 1;
	CD_REG(3) = 0x1f; // Acknowledge all IRQs
	CD_REG(2) = 0x1f; // Enable all IRQs
	CD_REG(0) = 0;
	CD_REG(3) = 0x00; // Clear any pending request

	CdlATV mix = { 0x80, 0x00, 0x80, 0x00 };
	CdMix(&mix);

	_last_mode    = 0;
	_ack_pending  = 0;
	_sync_pending = 0;
	_queue_head = 0;
	_queue_length = 0;
	_queue_active = 0;
	_queue_stalled = 0;

	_cd_override_callback = (CdlCB) 0;
	_cd_media_changed     = 1;

	// Initialize the drive.
	CdCommand(CdlNop, 0, 0, 0);
	CdCommand(CdlInit, 0, 0, 0);

	if (CdSync(0, 0) == CdlDiskError) {
		_sdk_log("setup error, bad disc/drive or no disc inserted\n");
		return 0;
	}

	CdCommand(CdlDemute, 0, 0, 0);
	_sdk_log("setup done\n");
	return 1;
}

/* Low-level command API */

int CdCommandF(CdlCommand cmd, const void *param, int length) {
	_sdk_validate_args(param || (length <= 0), -1);

	const uint8_t *_param = (const uint8_t *) param;

	_last_command = (uint8_t) cmd;
	_ack_pending  = 1;

	if (cmd <= CdlReadTOC) {
		if (_command_flags[cmd] & BLOCKING)
			_sync_pending = 1;

		// Keep track of the last mode and seek location set (so retries can be
		// attempted).
		if (cmd == CdlSetloc) {
			_last_pos.minute = _param[0];
			_last_pos.second = _param[1];
			_last_pos.sector = _param[2];
		} else if (cmd == CdlSetmode) {
			_last_mode = _param[0];
		}
	}

	// Request a command FIFO write.
	while (CD_REG(0) & 0x80)
		__asm__ volatile("");

	CD_REG(0) = 1;
	CD_REG(3) = 0x40; // Reset parameter buffer

	//while (CD_REG(0) & (1 << 5))
		//CD_REG(1);
	for (int i = 0; i < 50; i++)
		__asm__ volatile("");

	// Wait for the FIFO to become ready, then send the parameters followed by
	// the command index.
	while (CD_REG(0) & (1 << 7))
		__asm__ volatile("");

	CD_REG(0) = 0;
	for (; length > 0; length--)
		CD_REG(2) = *(_param++);

	CD_REG(0) = 0;
	CD_REG(1) = (uint8_t) cmd;
	return 1;
}

int CdCommand(CdlCommand cmd, const void *param, int length, uint8_t *result) {
	_sdk_validate_args(param || (length <= 0), -1);

	/*if (_ack_pending) {
		_sdk_log("CdCommand(0x%02x) failed, drive busy\n", cmd);
		return 0;
	}*/

	_result_ptr = result;
	CdCommandF(cmd, param, length);

	// Wait for the command to be acknowledged.
	for (int i = CD_ACK_TIMEOUT; i; i--) {
		if (!_ack_pending)
			return 1;
	}

	_sdk_log("CdCommand(0x%02x) failed, acknowledge timeout\n", cmd);
	return 0;
}

/* High-level command API */

int CdControlF(CdlCommand cmd, const void *param) {
	// Assume no parameters need to be passed if the command is unknown.
	uint8_t flags = (cmd <= CdlReadTOC) ? _command_flags[cmd] : 0;
	int length;

	if (flags & OPTIONAL) {
		// The command may optionally take a parameter.
		length = param ? (flags & 3) : 0;
	} else if (flags & SETLOC) {
		// The command takes no parameter, but the API allows specifying a
		// location to be sent to the drive as a separate CdlSetloc command.
		length = 0;
	} else {
		// The command takes a mandatory parameter or no parameter.
		length = flags & 3;
		if (length && !param) {
			_sdk_log("CdControl() param is required for command 0x%02x\n", cmd);
			return -1;
		}
	}

	if (_ack_callback && !_result_ptr) {
		_result_ptr = _ack_result;
	}
	if ((flags & SETLOC) && param) {
		CdCommandF(CdlSetloc, param, 3);
	}
	return CdCommandF(cmd, param, length);
}

int CdControl(CdlCommand cmd, const void *param, uint8_t *result) {
	/*if (_ack_pending) {
		_sdk_log("CdControl(0x%02x) failed, drive busy\n", cmd);
		return 0;
	}*/

	_result_ptr = result;
	CdControlF(cmd, param);

	// Wait for the command to be acknowledged.
	for (int i = CD_ACK_TIMEOUT; i; i--) {
		if (!_ack_pending)
			return 1;
	}

	_sdk_log("CdControl(0x%02x) failed, acknowledge timeout\n", cmd);
	return 0;
}

int CdControlB(CdlCommand cmd, const void *param, uint8_t *result) {
	int error = CdControl(cmd, param, result);
	if (error != 1)
		return error;

	error = CdSync(0, 0);
	return (error == CdlDiskError) ? 0 : 1;
}

/* Optional main-thread command queue. No commands are sent from the IRQ. */

int CdQueueCommand(CdlCommand cmd, const void *param, CdlQueueCB callback) {
	if (_queue_stalled || (_queue_length == CD_QUEUE_CAPACITY)) {
		return 0;
	}
	if ((cmd < CdlNop) || (cmd > CdlReadTOC) || (cmd == CdlReset) || (cmd == 0x17) || (cmd == 0x18)) {
		return 0;
	}

	uint8_t flags = _command_flags[cmd];
	// Location commands must be separate entries so each ACK is consumed.
	if ((flags & SETLOC) && param) {
		return 0;
	}
	int length = (flags & SETLOC) ? 0 : (flags & 3);
	if ((flags & OPTIONAL) && !param) {
		length = 0;
	}
	if (length && !param) {
		return 0;
	}

	QueuedCommand *entry = &_command_queue[(_queue_head + _queue_length) % CD_QUEUE_CAPACITY];
	entry->command = (uint8_t) cmd;
	entry->callback = callback;
	entry->has_parameters = length != 0;
	for (int i = 0; i < length; i++) {
		entry->parameters[i] = ((const uint8_t *) param)[i];
	}
	_queue_length++;
	return 1;
}

int CdQueueLength(void) {
	return _queue_stalled ? -1 : _queue_length;
}

int CdQueueUpdate(void) {
	if (_queue_stalled) {
		return -1;
	}
	if (!_queue_length) {
		return 0;
	}

	if (_queue_active) {
		CdlIntrResult result = _queue_result;
		uint8_t flags = _command_flags[_command_queue[_queue_head].command];
		if (result == CdlDiskError) {
			_queue_active = 0;
			_queue_length = 0;
			return -1;
		}
		if ((result == CdlNoIntr) || ((flags & BLOCKING) && (result != CdlComplete))) {
			if ((uint32_t) VSync(-1) - _queue_start_frame >= CD_QUEUE_TIMEOUT) {
				// A late IRQ must not be mistaken for a later command's reply.
				_queue_stalled = 1;
				return -1;
			}
			return _queue_length;
		}

		CdlQueueCB callback = _command_queue[_queue_head].callback;
		_queue_active = 0;
		_queue_head = (_queue_head + 1) % CD_QUEUE_CAPACITY;
		_queue_length--;
		if (callback) {
			callback(_queue_response);
		}
	}

	if (_queue_length && !_ack_pending && !_sync_pending) {
		const QueuedCommand *entry = &_command_queue[_queue_head];
		_queue_result = CdlNoIntr;
		_queue_start_frame = (uint32_t) VSync(-1);
		_result_ptr = _queue_response;
		_queue_active = 1;
		CdControlF((CdlCommand) entry->command, entry->has_parameters ? entry->parameters : (const void *) 0);
	}
	return _queue_length;
}

/* Other APIs and callback setters */

CdlIntrResult CdSync(int mode, uint8_t *result) {
	if (mode) {
		if (_sync_pending)
			return CdlNoIntr;

		if (result)
			*result = _last_status;
		if (_last_irq == CdlAcknowledge)
			return CdlComplete;

		return _last_irq;
	}

	for (int i = CD_SYNC_TIMEOUT; i; i--) {
		if (!_sync_pending)
			return CdSync(1, result); // :P
	}

	_sdk_log("CdSync() timeout\n");
	return -1;
}

CdlCommand CdLastCom(void) {
	return _last_command;
}

const CdlLOC *CdLastPos(void) {
	return &_last_pos;
}

int CdMode(void) {
	return _last_mode;
}

int CdStatus(void) {
	return _last_status;
}

CdlCB CdReadyCallback(CdlCB func) {
	FastEnterCriticalSection();

	CdlCB old_callback = _ready_callback;
	_ready_callback    = func;

	FastExitCriticalSection();
	return old_callback;
}

CdlCB CdAckCallback(CdlCB func) {
	FastEnterCriticalSection();

	CdlCB old_callback = _ack_callback;
	_ack_callback = func;

	FastExitCriticalSection();
	return old_callback;
}

CdlCB CdSyncCallback(CdlCB func) {
	FastEnterCriticalSection();

	CdlCB old_callback = _sync_callback;
	_sync_callback     = func;

	FastExitCriticalSection();
	return old_callback;
}

CdlCB CdAutoPauseCallback(CdlCB func) {
	FastEnterCriticalSection();

	CdlCB old_callback = _pause_callback;
	_pause_callback    = func;

	FastExitCriticalSection();
	return old_callback;
}
