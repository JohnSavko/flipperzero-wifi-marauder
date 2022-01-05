#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <lib/toolbox/args.h>
#include <furi_hal_usb_hid.h>
#include <storage/storage.h>
#include "bad_usb_script.h"

#define TAG "BadUSB"
#define WORKER_TAG TAG "Worker"
#define FILE_BUFFER_LEN 16

typedef enum {
    WorkerEvtReserved = (1 << 0),
    WorkerEvtToggle = (1 << 1),
    WorkerEvtEnd = (1 << 2),
    WorkerEvtConnect = (1 << 3),
    WorkerEvtDisconnect = (1 << 4),
} WorkerEvtFlags;

struct BadUsbScript {
    BadUsbState st;
    string_t file_path;
    uint32_t defdelay;
    FuriThread* thread;
    uint8_t file_buf[FILE_BUFFER_LEN + 1];
    uint8_t buf_start;
    uint8_t buf_len;
    bool file_end;
    string_t line;

    string_t line_prev;
    uint32_t repeat_cnt;
};

typedef struct {
    char* name;
    uint16_t keycode;
} DuckyKey;

static const DuckyKey ducky_keys[] = {
    {"CTRL-ALT", KEY_MOD_LEFT_CTRL | KEY_MOD_LEFT_ALT},
    {"CTRL-SHIFT", KEY_MOD_LEFT_CTRL | KEY_MOD_LEFT_SHIFT},
    {"ALT-SHIFT", KEY_MOD_LEFT_ALT | KEY_MOD_LEFT_SHIFT},
    {"ALT-GUI", KEY_MOD_LEFT_ALT | KEY_MOD_LEFT_GUI},

    {"CTRL", KEY_MOD_LEFT_CTRL},
    {"CONTROL", KEY_MOD_LEFT_CTRL},
    {"SHIFT", KEY_MOD_LEFT_SHIFT},
    {"ALT", KEY_MOD_LEFT_ALT},
    {"GUI", KEY_MOD_LEFT_GUI},
    {"WINDOWS", KEY_MOD_LEFT_GUI},

    {"DOWNARROW", KEY_DOWN_ARROW},
    {"DOWN", KEY_DOWN_ARROW},
    {"LEFTARROW", KEY_LEFT_ARROW},
    {"LEFT", KEY_LEFT_ARROW},
    {"RIGHTARROW", KEY_RIGHT_ARROW},
    {"RIGHT", KEY_RIGHT_ARROW},
    {"UPARROW", KEY_UP_ARROW},
    {"UP", KEY_UP_ARROW},

    {"ENTER", KEY_ENTER},
    {"BREAK", KEY_PAUSE},
    {"PAUSE", KEY_PAUSE},
    {"CAPSLOCK", KEY_CAPS_LOCK},
    {"DELETE", KEY_DELETE},
    {"BACKSPACE", KEY_BACKSPACE},
    {"END", KEY_END},
    {"ESC", KEY_ESC},
    {"ESCAPE", KEY_ESC},
    {"HOME", KEY_HOME},
    {"INSERT", KEY_INSERT},
    {"NUMLOCK", KEY_NUM_LOCK},
    {"PAGEUP", KEY_PAGE_UP},
    {"PAGEDOWN", KEY_PAGE_DOWN},
    {"PRINTSCREEN", KEY_PRINT},
    {"SCROLLOCK", KEY_SCROLL_LOCK},
    {"SPACE", KEY_SPACE},
    {"TAB", KEY_TAB},
    {"MENU", KEY_APPLICATION},
    {"APP", KEY_APPLICATION},

    {"F1", KEY_F1},
    {"F2", KEY_F2},
    {"F3", KEY_F3},
    {"F4", KEY_F4},
    {"F5", KEY_F5},
    {"F6", KEY_F6},
    {"F7", KEY_F7},
    {"F8", KEY_F8},
    {"F9", KEY_F9},
    {"F10", KEY_F10},
    {"F11", KEY_F11},
    {"F12", KEY_F12},
};

static const char ducky_cmd_comment[] = {"REM"};
static const char ducky_cmd_delay[] = {"DELAY "};
static const char ducky_cmd_string[] = {"STRING "};
static const char ducky_cmd_defdelay_1[] = {"DEFAULT_DELAY "};
static const char ducky_cmd_defdelay_2[] = {"DEFAULTDELAY "};
static const char ducky_cmd_repeat[] = {"REPEAT "};

static const char ducky_cmd_altchar[] = {"ALTCHAR "};
static const char ducky_cmd_altstr_1[] = {"ALTSTRING "};
static const char ducky_cmd_altstr_2[] = {"ALTCODE "};

static const uint8_t numpad_keys[10] = {
    KEYPAD_0,
    KEYPAD_1,
    KEYPAD_2,
    KEYPAD_3,
    KEYPAD_4,
    KEYPAD_5,
    KEYPAD_6,
    KEYPAD_7,
    KEYPAD_8,
    KEYPAD_9,
};

static bool ducky_get_number(const char* param, uint32_t* val) {
    uint32_t value = 0;
    if(sscanf(param, "%lu", &value) == 1) {
        *val = value;
        return true;
    }
    return false;
}

static uint32_t ducky_get_command_len(const char* line) {
    uint32_t len = strlen(line);
    for(uint32_t i = 0; i < len; i++) {
        if(line[i] == ' ') return i;
    }
    return 0;
}

static void ducky_numlock_on() {
    if((furi_hal_hid_get_led_state() & HID_KB_LED_NUM) == 0) {
        furi_hal_hid_kb_press(KEY_NUM_LOCK);
        furi_hal_hid_kb_release(KEY_NUM_LOCK);
    }
}

static bool ducky_numpad_press(const char num) {
    if((num < '0') || (num > '9')) return false;

    uint16_t key = numpad_keys[num - '0'];
    furi_hal_hid_kb_press(key);
    furi_hal_hid_kb_release(key);

    return true;
}

static bool ducky_altchar(const char* charcode) {
    uint8_t i = 0;
    bool state = false;

    //TODO: numlock

    FURI_LOG_I(WORKER_TAG, "char %s", charcode);

    furi_hal_hid_kb_press(KEY_MOD_LEFT_ALT);

    while((charcode[i] != ' ') && (charcode[i] != '\n') && (charcode[i] != '\0')) {
        state = ducky_numpad_press(charcode[i]);
        if(state == false) break;
        i++;
    }

    furi_hal_hid_kb_release(KEY_MOD_LEFT_ALT);
    return state;
}

static bool ducky_altstring(const char* param) {
    uint32_t i = 0;
    bool state = false;

    while(param[i] != '\0') {
        if((param[i] < ' ') || (param[i] > '~')) {
            i++;
            continue; // Skip non-printable chars
        }

        char temp_str[4];
        snprintf(temp_str, 4, "%u", param[i]);

        state = ducky_altchar(temp_str);
        if(state == false) break;
        i++;
    }
    return state;
}

static bool ducky_string(const char* param) {
    uint32_t i = 0;
    while(param[i] != '\0') {
        furi_hal_hid_kb_press(HID_ASCII_TO_KEY(param[i]));
        furi_hal_hid_kb_release(HID_ASCII_TO_KEY(param[i]));
        i++;
    }
    return true;
}

static uint16_t ducky_get_keycode(const char* param, bool accept_chars) {
    for(uint8_t i = 0; i < (sizeof(ducky_keys) / sizeof(ducky_keys[0])); i++) {
        uint8_t key_cmd_len = strlen(ducky_keys[i].name);
        if((strncmp(param, ducky_keys[i].name, key_cmd_len) == 0) &&
           ((param[key_cmd_len] == ' ') || (param[key_cmd_len] == '\n') ||
            (param[key_cmd_len] == '\0'))) {
            return ducky_keys[i].keycode;
        }
    }
    if((accept_chars) && (strlen(param) > 0)) {
        return (HID_ASCII_TO_KEY(param[0]) & 0xFF);
    }
    return 0;
}

static int32_t ducky_parse_line(BadUsbScript* bad_usb, string_t line) {
    uint32_t line_len = string_size(line);
    const char* line_tmp = string_get_cstr(line);
    bool state = false;

    for(uint32_t i = 0; i < line_len; i++) {
        if((line_tmp[i] != ' ') && (line_tmp[i] != '\t') && (line_tmp[i] != '\n')) {
            line_tmp = &line_tmp[i];
            break; // Skip spaces and tabs
        }
        if(i == line_len - 1) return 0; // Skip empty lines
    }

    FURI_LOG_I(WORKER_TAG, "line:%s", line_tmp);

    // General commands
    if(strncmp(line_tmp, ducky_cmd_comment, strlen(ducky_cmd_comment)) == 0) {
        // REM - comment line
        return (0);
    } else if(strncmp(line_tmp, ducky_cmd_delay, strlen(ducky_cmd_delay)) == 0) {
        // DELAY
        line_tmp = &line_tmp[ducky_get_command_len(line_tmp) + 1];
        uint32_t delay_val = 0;
        state = ducky_get_number(line_tmp, &delay_val);
        if((state) && (delay_val > 0)) {
            return (int32_t)delay_val;
        }
        return (-1);
    } else if(
        (strncmp(line_tmp, ducky_cmd_defdelay_1, strlen(ducky_cmd_defdelay_1)) == 0) ||
        (strncmp(line_tmp, ducky_cmd_defdelay_2, strlen(ducky_cmd_defdelay_2)) == 0)) {
        // DEFAULT_DELAY
        line_tmp = &line_tmp[ducky_get_command_len(line_tmp) + 1];
        state = ducky_get_number(line_tmp, &bad_usb->defdelay);
        return (state) ? (0) : (-1);
    } else if(strncmp(line_tmp, ducky_cmd_string, strlen(ducky_cmd_string)) == 0) {
        // STRING
        line_tmp = &line_tmp[ducky_get_command_len(line_tmp) + 1];
        state = ducky_string(line_tmp);
        return (state) ? (0) : (-1);
    } else if(strncmp(line_tmp, ducky_cmd_altchar, strlen(ducky_cmd_altchar)) == 0) {
        // ALTCHAR
        line_tmp = &line_tmp[ducky_get_command_len(line_tmp) + 1];
        ducky_numlock_on();
        state = ducky_altchar(line_tmp);
        return (state) ? (0) : (-1);
    } else if(
        (strncmp(line_tmp, ducky_cmd_altstr_1, strlen(ducky_cmd_altstr_1)) == 0) ||
        (strncmp(line_tmp, ducky_cmd_altstr_2, strlen(ducky_cmd_altstr_2)) == 0)) {
        // ALTSTRING
        line_tmp = &line_tmp[ducky_get_command_len(line_tmp) + 1];
        ducky_numlock_on();
        state = ducky_altstring(line_tmp);
        return (state) ? (0) : (-1);
    } else if(strncmp(line_tmp, ducky_cmd_repeat, strlen(ducky_cmd_repeat)) == 0) {
        // REPEAT
        line_tmp = &line_tmp[ducky_get_command_len(line_tmp) + 1];
        state = ducky_get_number(line_tmp, &bad_usb->repeat_cnt);
        return (state) ? (0) : (-1);
    } else {
        // Special keys + modifiers
        uint16_t key = ducky_get_keycode(line_tmp, false);
        if(key == KEY_NONE) return (-1);
        if((key & 0xFF00) != 0) {
            // It's a modifier key
            line_tmp = &line_tmp[ducky_get_command_len(line_tmp) + 1];
            key |= ducky_get_keycode(line_tmp, true);
        }
        furi_hal_hid_kb_press(key);
        furi_hal_hid_kb_release(key);
        return (0);
    }
    return (-1);
}

static bool ducky_script_preload(BadUsbScript* bad_usb, File* script_file) {
    uint8_t ret = 0;
    uint32_t line_len = 0;

    do {
        ret = storage_file_read(script_file, bad_usb->file_buf, FILE_BUFFER_LEN);
        for(uint16_t i = 0; i < ret; i++) {
            if(bad_usb->file_buf[i] == '\n' && line_len > 0) {
                bad_usb->st.line_nb++;
                line_len = 0;
            } else {
                line_len++;
            }
        }
        if(storage_file_eof(script_file)) {
            if(line_len > 0) {
                bad_usb->st.line_nb++;
                break;
            }
        }
    } while(ret > 0);

    storage_file_seek(script_file, 0, true);

    return true;
}

static int32_t ducky_script_execute_next(BadUsbScript* bad_usb, File* script_file) {
    int32_t delay_val = 0;

    if(bad_usb->repeat_cnt > 0) {
        bad_usb->repeat_cnt--;
        delay_val = ducky_parse_line(bad_usb, bad_usb->line_prev);
        if(delay_val < 0) {
            bad_usb->st.error_line = bad_usb->st.line_cur - 1;
            FURI_LOG_E(WORKER_TAG, "Unknown command at line %lu", bad_usb->st.line_cur - 1);
            return (-1);
        } else {
            return (delay_val + bad_usb->defdelay);
        }
    }

    string_set(bad_usb->line_prev, bad_usb->line);
    string_reset(bad_usb->line);

    while(1) {
        if(bad_usb->buf_len == 0) {
            bad_usb->buf_len = storage_file_read(script_file, bad_usb->file_buf, FILE_BUFFER_LEN);
            if(storage_file_eof(script_file)) {
                if((bad_usb->buf_len < FILE_BUFFER_LEN) && (bad_usb->file_end == false)) {
                    bad_usb->file_buf[bad_usb->buf_len] = '\n';
                    bad_usb->buf_len++;
                    bad_usb->file_end = true;
                }
            }

            bad_usb->buf_start = 0;
            if(bad_usb->buf_len == 0) return (-2);
        }
        for(uint8_t i = bad_usb->buf_start; i < (bad_usb->buf_start + bad_usb->buf_len); i++) {
            if(bad_usb->file_buf[i] == '\n' && string_size(bad_usb->line) > 0) {
                bad_usb->st.line_cur++;
                bad_usb->buf_len = bad_usb->buf_len + bad_usb->buf_start - (i + 1);
                bad_usb->buf_start = i + 1;
                delay_val = ducky_parse_line(bad_usb, bad_usb->line);
                if(delay_val < 0) {
                    bad_usb->st.error_line = bad_usb->st.line_cur;
                    FURI_LOG_E(WORKER_TAG, "Unknown command at line %lu", bad_usb->st.line_cur);
                    return (-1);
                } else {
                    return (delay_val + bad_usb->defdelay);
                }
            } else {
                string_push_back(bad_usb->line, bad_usb->file_buf[i]);
            }
        }
        bad_usb->buf_len = 0;
        if(bad_usb->file_end) return (-2);
    }

    return 0;
}

static void bad_usb_hid_state_callback(bool state, void* context) {
    furi_assert(context);
    BadUsbScript* bad_usb = context;

    if(state == true)
        osThreadFlagsSet(furi_thread_get_thread_id(bad_usb->thread), WorkerEvtConnect);
    else
        osThreadFlagsSet(furi_thread_get_thread_id(bad_usb->thread), WorkerEvtDisconnect);
}

static int32_t bad_usb_worker(void* context) {
    BadUsbScript* bad_usb = context;

    BadUsbWorkerState worker_state = BadUsbStateInit;
    int32_t delay_val = 0;

    FURI_LOG_I(WORKER_TAG, "Init");
    File* script_file = storage_file_alloc(furi_record_open("storage"));
    string_init(bad_usb->line);
    string_init(bad_usb->line_prev);

    furi_hal_hid_set_state_callback(bad_usb_hid_state_callback, bad_usb);

    while(1) {
        if(worker_state == BadUsbStateInit) { // State: initialization
            if(storage_file_open(
                   script_file,
                   string_get_cstr(bad_usb->file_path),
                   FSAM_READ,
                   FSOM_OPEN_EXISTING)) {
                if((ducky_script_preload(bad_usb, script_file)) && (bad_usb->st.line_nb > 0)) {
                    if(furi_hal_hid_is_connected()) {
                        worker_state = BadUsbStateIdle; // Ready to run
                    } else {
                        worker_state = BadUsbStateNotConnected; // USB not connected
                    }
                } else {
                    worker_state = BadUsbStateScriptError; // Script preload error
                }
            } else {
                FURI_LOG_E(WORKER_TAG, "File open error");
                worker_state = BadUsbStateFileError; // File open error
            }
            bad_usb->st.state = worker_state;

        } else if(worker_state == BadUsbStateNotConnected) { // State: USB not connected
            uint32_t flags =
                osThreadFlagsWait(WorkerEvtEnd | WorkerEvtConnect, osFlagsWaitAny, osWaitForever);
            furi_check((flags & osFlagsError) == 0);
            if(flags & WorkerEvtEnd) {
                break;
            } else if(flags & WorkerEvtConnect) {
                worker_state = BadUsbStateIdle; // Ready to run
            }
            bad_usb->st.state = worker_state;

        } else if(worker_state == BadUsbStateIdle) { // State: ready to start
            uint32_t flags = osThreadFlagsWait(
                WorkerEvtEnd | WorkerEvtToggle | WorkerEvtDisconnect,
                osFlagsWaitAny,
                osWaitForever);
            furi_check((flags & osFlagsError) == 0);
            if(flags & WorkerEvtEnd) {
                break;
            } else if(flags & WorkerEvtToggle) { // Start executing script
                delay_val = 0;
                bad_usb->buf_len = 0;
                bad_usb->st.line_cur = 0;
                bad_usb->defdelay = 0;
                bad_usb->repeat_cnt = 0;
                bad_usb->file_end = false;
                storage_file_seek(script_file, 0, true);
                worker_state = BadUsbStateRunning;
            } else if(flags & WorkerEvtDisconnect) {
                worker_state = BadUsbStateNotConnected; // USB disconnected
            }
            bad_usb->st.state = worker_state;

        } else if(worker_state == BadUsbStateRunning) { // State: running
            uint16_t delay_cur = (delay_val > 1000) ? (1000) : (delay_val);
            uint32_t flags = osThreadFlagsWait(
                WorkerEvtEnd | WorkerEvtToggle | WorkerEvtDisconnect, osFlagsWaitAny, delay_cur);
            delay_val -= delay_cur;
            if(!(flags & osFlagsError)) {
                if(flags & WorkerEvtEnd) {
                    break;
                } else if(flags & WorkerEvtToggle) {
                    worker_state = BadUsbStateIdle; // Stop executing script
                    furi_hal_hid_kb_release_all();
                } else if(flags & WorkerEvtDisconnect) {
                    worker_state = BadUsbStateNotConnected; // USB disconnected
                    furi_hal_hid_kb_release_all();
                }
                bad_usb->st.state = worker_state;
                continue;
            } else if((flags == osFlagsErrorTimeout) || (flags == osFlagsErrorResource)) {
                if(delay_val > 0) {
                    bad_usb->st.delay_remain--;
                    continue;
                }
                bad_usb->st.state = BadUsbStateRunning;
                delay_val = ducky_script_execute_next(bad_usb, script_file);
                if(delay_val == -1) { // Script error
                    delay_val = 0;
                    worker_state = BadUsbStateScriptError;
                    bad_usb->st.state = worker_state;
                } else if(delay_val == -2) { // End of script
                    delay_val = 0;
                    worker_state = BadUsbStateIdle;
                    bad_usb->st.state = BadUsbStateDone;
                    furi_hal_hid_kb_release_all();
                    continue;
                } else if(delay_val > 1000) {
                    bad_usb->st.state = BadUsbStateDelay; // Show long delays
                    bad_usb->st.delay_remain = delay_val / 1000;
                }
            } else {
                furi_check((flags & osFlagsError) == 0);
            }

        } else if(
            (worker_state == BadUsbStateFileError) ||
            (worker_state == BadUsbStateScriptError)) { // State: error
            uint32_t flags = osThreadFlagsWait(
                WorkerEvtEnd, osFlagsWaitAny, osWaitForever); // Waiting for exit command
            furi_check((flags & osFlagsError) == 0);
            if(flags & WorkerEvtEnd) {
                break;
            }
        }
    }

    furi_hal_hid_set_state_callback(NULL, NULL);

    storage_file_close(script_file);
    storage_file_free(script_file);
    string_clear(bad_usb->line);
    string_clear(bad_usb->line_prev);

    FURI_LOG_I(WORKER_TAG, "End");

    return 0;
}

BadUsbScript* bad_usb_script_open(string_t file_path) {
    furi_assert(file_path);

    BadUsbScript* bad_usb = furi_alloc(sizeof(BadUsbScript));
    string_init(bad_usb->file_path);
    string_set(bad_usb->file_path, file_path);

    bad_usb->st.state = BadUsbStateInit;

    bad_usb->thread = furi_thread_alloc();
    furi_thread_set_name(bad_usb->thread, "BadUsbWorker");
    furi_thread_set_stack_size(bad_usb->thread, 2048);
    furi_thread_set_context(bad_usb->thread, bad_usb);
    furi_thread_set_callback(bad_usb->thread, bad_usb_worker);

    furi_thread_start(bad_usb->thread);
    return bad_usb;
}

void bad_usb_script_close(BadUsbScript* bad_usb) {
    furi_assert(bad_usb);
    osThreadFlagsSet(furi_thread_get_thread_id(bad_usb->thread), WorkerEvtEnd);
    furi_thread_join(bad_usb->thread);
    furi_thread_free(bad_usb->thread);
    string_clear(bad_usb->file_path);
    free(bad_usb);
}

void bad_usb_script_toggle(BadUsbScript* bad_usb) {
    furi_assert(bad_usb);
    osThreadFlagsSet(furi_thread_get_thread_id(bad_usb->thread), WorkerEvtToggle);
}

BadUsbState* bad_usb_script_get_state(BadUsbScript* bad_usb) {
    furi_assert(bad_usb);
    return &(bad_usb->st);
}
