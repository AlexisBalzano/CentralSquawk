//
// NeoChat public API for other EuroScope plugins.
//
// No link dependency: resolve the functions at runtime from the loaded NeoChat.dll, e.g.
//
//     using AddMessageFn = int (__cdecl*)(const char*, const char*, const char*, int);
//     if (HMODULE neochat = GetModuleHandleA("NeoChat.dll")) {
//         if (auto add = reinterpret_cast<AddMessageFn>(GetProcAddress(neochat, "NeoChat_AddMessage"))) {
//             add("MyPlugin", "MyPlugin", "Hello from MyPlugin", 0);
//         }
//     }
//
// Rules:
//  - Call from EuroScope's UI thread only (plugin callbacks such as OnTimer are fine).
//  - NeoChat can be unloaded at any time: resolve the functions again before each use
//    (GetModuleHandleA + GetProcAddress are cheap) instead of caching the pointers.
//  - Strings are UTF-8, copied by NeoChat before returning.
//  - Messages you send with CPlugIn::DisplayUserMessage already show in NeoChat; use this API
//    for formatted text or to register your dot-commands for autocomplete.
//

#ifndef NEOCHAT_API_H
#define NEOCHAT_API_H

#define NEOCHAT_API_VERSION 2   // 2: NeoChat_SetInputText

// Return codes
#define NEOCHAT_OK              0
#define NEOCHAT_ERR_NOT_READY  -1  // NeoChat disabled (wrong EuroScope version) or shutting down
#define NEOCHAT_ERR_ARGUMENT   -2  // invalid / empty argument

// Fragment color meaning "use NeoChat's normal text color"
#define NEOCHAT_DEFAULT_COLOR  0xFFFFFFFFu

// NeoChat_SetInputText flags
#define NEOCHAT_INPUT_FOCUS    0x1  // focus the input box (shows the chat window if hidden)
#define NEOCHAT_INPUT_APPEND   0x2  // append to the current input instead of replacing it

typedef struct NeoChatFragment {
    const char*  text;       // UTF-8, required
    unsigned int color;      // COLORREF (0x00BBGGRR) or NEOCHAT_DEFAULT_COLOR
    int          bold;       // non-zero = bold
    int          sizeDelta;  // font size offset in pixels (0 = normal)
} NeoChatFragment;

#ifdef __cplusplus
extern "C" {
#endif

// NEOCHAT_API_VERSION of the loaded NeoChat
typedef int (__cdecl *NeoChat_GetApiVersion_t)(void);

// Plain message in tab `tab` (created if needed). `sender` may be NULL/"" for none.
// NeoChat highlights callsigns, frequencies, links... like any other message.
// unread != 0: tab blinks and plays a sound (unless muted).
typedef int (__cdecl *NeoChat_AddMessage_t)(const char* tab, const char* sender, const char* text, int unread);

// Message made of `count` formatted fragments (no automatic highlighting).
typedef int (__cdecl *NeoChat_AddFormattedMessage_t)(const char* tab, const char* sender,
                                                     const NeoChatFragment* fragments, int count, int unread);

// Register / unregister one of your dot-commands (".mycmd"): autocompleted and never shown as invalid.
// Multi-word commands are allowed (".mycmd sub"): completed as a whole, and ".mycmd" alone is valid.
// Arguments can be shown as placeholders: ".mycmd sub <callsign> <level>". The completion shows them, and
// the user selects each one in turn with NeoChat's next_parameter key (default Tab) to type over it.
typedef int (__cdecl *NeoChat_RegisterCommand_t)(const char* command);
typedef int (__cdecl *NeoChat_UnregisterCommand_t)(const char* command);

// (API version 2) Pre-fill NeoChat's input box with `text` (UTF-8), for the user to review and send,
// e.g. a standard message for a text pilot. `tab`: NULL/"" keeps the current tab, "@" selects the
// primary frequency tab, any other name opens/selects that tab (".msg"-style private tab). A "$1".."$9"
// placeholder in `text` is selected so the user types over it (next one with the next_parameter key).
// Nothing is sent: the user presses Enter. Fallback without NeoChat: write into EuroScope's own
// command line (or skip).
typedef int (__cdecl *NeoChat_SetInputText_t)(const char* text, const char* tab, unsigned int flags);

#ifdef __cplusplus
}
#endif

#endif // NEOCHAT_API_H
