//go:build windows

package main

// The window version of the host (Windows only): a small window with the
// shared folder, name and port, a Start/Stop button, the inbox (receiving what the
// DOS client sends) with its own Start/Stop button, the address to type into
// the DOS client and a list of what the clients do. Closing the window hides
// it in the notification area (tray); "Exit" in the tray menu ends the program.
//
// It talks to Windows directly through the standard library (syscall), so no
// extra package is needed. Build it with
//
//	go build -ldflags "-H=windowsgui -s -w" -o echost.exe
//
// to get a program without a console window. Started with -console it works
// as the console version.

import (
	"context"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

const guiAvailable = true

// ---------------------------------------------------------------------
// Windows API

var (
	user32   = syscall.NewLazyDLL("user32.dll")
	kernel32 = syscall.NewLazyDLL("kernel32.dll")
	gdi32    = syscall.NewLazyDLL("gdi32.dll")
	shell32  = syscall.NewLazyDLL("shell32.dll")
	ole32    = syscall.NewLazyDLL("ole32.dll")

	pRegisterClassExW       = user32.NewProc("RegisterClassExW")
	pCreateWindowExW        = user32.NewProc("CreateWindowExW")
	pDefWindowProcW         = user32.NewProc("DefWindowProcW")
	pGetMessageW            = user32.NewProc("GetMessageW")
	pTranslateMessage       = user32.NewProc("TranslateMessage")
	pDispatchMessageW       = user32.NewProc("DispatchMessageW")
	pIsDialogMessageW       = user32.NewProc("IsDialogMessageW")
	pPostQuitMessage        = user32.NewProc("PostQuitMessage")
	pPostMessageW           = user32.NewProc("PostMessageW")
	pSendMessageW           = user32.NewProc("SendMessageW")
	pShowWindow             = user32.NewProc("ShowWindow")
	pDestroyWindow          = user32.NewProc("DestroyWindow")
	pSetForegroundWindow    = user32.NewProc("SetForegroundWindow")
	pSetWindowTextW         = user32.NewProc("SetWindowTextW")
	pGetWindowTextW         = user32.NewProc("GetWindowTextW")
	pGetWindowTextLengthW   = user32.NewProc("GetWindowTextLengthW")
	pEnableWindow           = user32.NewProc("EnableWindow")
	pSetTimer               = user32.NewProc("SetTimer")
	pKillTimer              = user32.NewProc("KillTimer")
	pLoadCursorW            = user32.NewProc("LoadCursorW")
	pLoadIconW              = user32.NewProc("LoadIconW")
	pMessageBoxW            = user32.NewProc("MessageBoxW")
	pCreatePopupMenu        = user32.NewProc("CreatePopupMenu")
	pAppendMenuW            = user32.NewProc("AppendMenuW")
	pTrackPopupMenu         = user32.NewProc("TrackPopupMenu")
	pDestroyMenu            = user32.NewProc("DestroyMenu")
	pGetCursorPos           = user32.NewProc("GetCursorPos")
	pGetSystemMetrics       = user32.NewProc("GetSystemMetrics")
	pAdjustWindowRectEx     = user32.NewProc("AdjustWindowRectEx")
	pFindWindowW            = user32.NewProc("FindWindowW")
	pRegisterWindowMessageW = user32.NewProc("RegisterWindowMessageW")
	pCreateIconIndirect     = user32.NewProc("CreateIconIndirect")
	pSetProcessDPIAware     = user32.NewProc("SetProcessDPIAware")
	pEnumThreadWindows      = user32.NewProc("EnumThreadWindows")
	pGetDC                  = user32.NewProc("GetDC")
	pReleaseDC              = user32.NewProc("ReleaseDC")
	pGetModuleHandleW       = kernel32.NewProc("GetModuleHandleW")
	pGetCurrentThreadID     = kernel32.NewProc("GetCurrentThreadId")
	pCreateMutexW           = kernel32.NewProc("CreateMutexW")
	pFreeConsole            = kernel32.NewProc("FreeConsole")
	pAttachConsole          = kernel32.NewProc("AttachConsole")
	pCreateActCtxW          = kernel32.NewProc("CreateActCtxW")
	pActivateActCtx         = kernel32.NewProc("ActivateActCtx")
	pCreateFontW            = gdi32.NewProc("CreateFontW")
	pCreateBitmap           = gdi32.NewProc("CreateBitmap")
	pDeleteObject           = gdi32.NewProc("DeleteObject")
	pGetDeviceCaps          = gdi32.NewProc("GetDeviceCaps")
	pShellNotifyIconW       = shell32.NewProc("Shell_NotifyIconW")
	pSHBrowseForFolderW     = shell32.NewProc("SHBrowseForFolderW")
	pSHGetPathFromIDListW   = shell32.NewProc("SHGetPathFromIDListW")
	pOleInitialize          = ole32.NewProc("OleInitialize")
	pCoTaskMemFree          = ole32.NewProc("CoTaskMemFree")
)

const (
	wsOverlapped   = 0x00000000
	wsCaption      = 0x00C00000
	wsSysMenu      = 0x00080000
	wsMinimizeBox  = 0x00020000
	wsChild        = 0x40000000
	wsVisible      = 0x10000000
	wsTabStop      = 0x00010000
	wsVScroll      = 0x00200000
	wsBorder       = 0x00800000
	wsExClientEdge = 0x00000200

	bsPushButton   = 0x0000
	bsAutoCheckbox = 0x0003
	esAutoHScroll  = 0x0080
	esReadOnly     = 0x0800
	esNumber       = 0x2000
	ssNoPrefix     = 0x0080
	lbsNoIntegral  = 0x0100

	wmCreate      = 0x0001
	wmDestroy     = 0x0002
	wmClose       = 0x0010
	wmSetFont     = 0x0030
	wmCommand     = 0x0111
	wmTimer       = 0x0113
	wmNull        = 0x0000
	wmLButtonUp   = 0x0202
	wmLButtonDbl  = 0x0203
	wmRButtonUp   = 0x0205
	wmApp         = 0x8000
	wmTray        = wmApp + 1
	wmShowMe      = wmApp + 2
	lbAddString   = 0x0180
	lbDeleteStr   = 0x0182
	lbGetCount    = 0x018B
	lbSetTopIndex = 0x0197
	lbGetItemH    = 0x01A1
	bmGetCheck    = 0x00F0
	bmSetCheck    = 0x00F1
	emSetLimit    = 0x00C5

	swHide = 0
	swShow = 5

	nimAdd     = 0
	nimModify  = 1
	nimDelete  = 2
	nifMessage = 1
	nifIcon    = 2
	nifTip     = 4

	mfString    = 0x0000
	mfSeparator = 0x0800
	tpmRightBtn = 0x0002

	colorBtnFace = 15
	idcArrow     = 32512
	idiApp       = 32512

	mbOK            = 0x00000000
	mbYesNo         = 0x00000004
	mbIconError     = 0x00000010
	mbIconQuestion  = 0x00000020
	mbIconWarning   = 0x00000030
	mbIconInfo      = 0x00000040
	mbDefButton2    = 0x00000100
	mbSetForeground = 0x00010000
	mbTopmost       = 0x00040000

	idYes = 6
	idNo  = 7
)

// control and menu numbers
const (
	idBrowse = 101
	idFolder = 108
	idName   = 102
	idPort   = 103
	idStart  = 104
	idHidden = 105
	idHide   = 106
	idExit   = 107

	idInBrowse = 109
	idInFolder = 110
	idQuota    = 111
	idAsk      = 112
	idRxStart  = 113

	idTrayOpen   = 201
	idTrayToggle = 202
	idTrayExit   = 203
	idTrayRx     = 204

	timerID = 1
)

const windowClass = "EasyCommanderHost"
const windowTitle = "Easy Commander host" + titleSuffix

// call runs a Windows function. Arguments are numbers or pointers (*uint16
// text, unsafe.Pointer to a structure); they stay alive until it returns.
func call(p *syscall.LazyProc, args ...any) uintptr {
	a := make([]uintptr, len(args))
	for i, v := range args {
		switch x := v.(type) {
		case uintptr:
			a[i] = x
		case int:
			a[i] = uintptr(x)
		case int32:
			a[i] = uintptr(x)
		case uint32:
			a[i] = uintptr(x)
		case *uint16:
			a[i] = uintptr(unsafe.Pointer(x))
		case unsafe.Pointer:
			a[i] = uintptr(x)
		default:
			panic(fmt.Sprintf("call: bad argument type %T", v))
		}
	}
	r, _, _ := p.Call(a...)
	runtime.KeepAlive(args)
	return r
}

func wstr(s string) *uint16 {
	p, _ := syscall.UTF16PtrFromString(s)
	return p
}

type point struct{ x, y int32 }
type rect struct{ left, top, right, bottom int32 }

type msgT struct {
	hwnd    uintptr
	message uint32
	wParam  uintptr
	lParam  uintptr
	time    uint32
	pt      point
	private uint32
}

type wndClassEx struct {
	cbSize        uint32
	style         uint32
	lpfnWndProc   uintptr
	cbClsExtra    int32
	cbWndExtra    int32
	hInstance     uintptr
	hIcon         uintptr
	hCursor       uintptr
	hbrBackground uintptr
	lpszMenuName  *uint16
	lpszClassName *uint16
	hIconSm       uintptr
}

type notifyIconData struct {
	cbSize           uint32
	hWnd             uintptr
	uID              uint32
	uFlags           uint32
	uCallbackMessage uint32
	hIcon            uintptr
	szTip            [128]uint16
	dwState          uint32
	dwStateMask      uint32
	szInfo           [256]uint16
	uVersion         uint32
	szInfoTitle      [64]uint16
	dwInfoFlags      uint32
	guidItem         [16]byte
	hBalloonIcon     uintptr
}

type iconInfo struct {
	fIcon    uint32
	xHotspot uint32
	yHotspot uint32
	hbmMask  uintptr
	hbmColor uintptr
}

type browseInfo struct {
	hwndOwner      uintptr
	pidlRoot       uintptr
	pszDisplayName *uint16
	lpszTitle      *uint16
	ulFlags        uint32
	lpfn           uintptr
	lParam         uintptr
	iImage         int32
}

type actCtx struct {
	cbSize                 uint32
	dwFlags                uint32
	lpSource               *uint16
	wProcessorArchitecture uint16
	wLangID                uint16
	lpAssemblyDirectory    *uint16
	lpResourceName         *uint16
	lpApplicationName      *uint16
	hModule                uintptr
}

// ---------------------------------------------------------------------
// State of the window

var (
	host    = NewHost()
	ring    logRing
	ringPos int
	cfg     config
	cfgPath string

	hInst, hMain, hFont      uintptr
	hIconBig, hIconSmall     uintptr
	hEdFolder, hEdName       uintptr
	hEdPort, hBtnBrowse      uintptr
	hBtnStart, hLblStatus    uintptr
	hLblIPs, hList, hChkHide uintptr
	hEdInbox, hBtnInBrowse   uintptr
	hEdQuota, hChkAsk        uintptr
	hBtnRx, hLblRx           uintptr

	dpi               = 96
	msgTaskbarCreated uintptr
	lastStatus        string
	lastRxStatus      string
	lastIPs           string
)

// scale converts pixels at 96 dpi to the real pixels of this screen.
func sc(n int) int { return n * dpi / 96 }

// ---------------------------------------------------------------------
// Helpers

func setText(h uintptr, s string) {
	call(pSetWindowTextW, h, wstr(s))
}

func getText(h uintptr) string {
	n := int(call(pGetWindowTextLengthW, h))
	buf := make([]uint16, n+2)
	call(pGetWindowTextW, h, unsafe.Pointer(&buf[0]), len(buf))
	return syscall.UTF16ToString(buf)
}

func msgBox(text string, flags uintptr) {
	call(pMessageBoxW, hMain, wstr(text), wstr(windowTitle), flags)
}

// askBox is a message box with an answer: it returns the button number.
func askBox(text string, flags uintptr) int {
	return int(call(pMessageBoxW, hMain, wstr(text), wstr(windowTitle), flags))
}

func control(class, text string, style, exStyle uint32, x, y, w, h, id int) uintptr {
	r := call(pCreateWindowExW, exStyle, wstr(class), wstr(text),
		style|wsChild|wsVisible, sc(x), sc(y), sc(w), sc(h),
		hMain, id, hInst, 0)
	call(pSendMessageW, r, wmSetFont, hFont, 1)
	return r
}

// ---------------------------------------------------------------------
// Look: visual styles, icon

// Without a manifest the buttons look like Windows 95. A manifest written to
// a temporary file and activated at run time gives the current look.
func enableVisualStyles() {
	manifest := `<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
<dependency><dependentAssembly>
<assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0" processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*"/>
</dependentAssembly></dependency>
</assembly>`
	path := filepath.Join(os.TempDir(), fmt.Sprintf("echost-%d.manifest", os.Getpid()))
	if os.WriteFile(path, []byte(manifest), 0o600) != nil {
		return
	}
	defer os.Remove(path)

	ctx := actCtx{lpSource: wstr(path)}
	ctx.cbSize = uint32(unsafe.Sizeof(ctx))
	h := call(pCreateActCtxW, unsafe.Pointer(&ctx))
	if h == ^uintptr(0) {
		return
	}
	var cookie uintptr
	if call(pActivateActCtx, h, unsafe.Pointer(&cookie)) == 0 {
		return
	}
	syscall.LoadLibrary("comctl32.dll") // must be loaded while the manifest is active
}

// makeIcon draws a small two-panel icon (blue, cyan frames, key bar).
func makeIcon(size int) uintptr {
	pix := make([]byte, size*size*4)
	for y := 0; y < size; y++ {
		for x := 0; x < size; x++ {
			gx, gy := x*32/size, y*32/size
			r, g, b := byte(0), byte(0), byte(170)
			switch {
			case gy >= 26:
				r, g, b = 0, 170, 170
				if gx%6 == 0 {
					r, g, b = 0, 0, 0
				}
			case gy >= 3 && gy <= 23 && ((gx >= 2 && gx <= 14) || (gx >= 17 && gx <= 29)):
				edgeX := gx == 2 || gx == 14 || gx == 17 || gx == 29
				edgeY := gy == 3 || gy == 23
				switch {
				case edgeX || edgeY:
					r, g, b = 85, 255, 255
				case gx < 16 && gy >= 10 && gy <= 12:
					r, g, b = 0, 170, 170 // the bar on the selected row
				case gy >= 6 && gy%4 == 2:
					r, g, b = 255, 255, 255
				}
			}
			i := (y*size + x) * 4
			pix[i], pix[i+1], pix[i+2], pix[i+3] = b, g, r, 255
		}
	}
	mask := make([]byte, ((size+15)/16)*2*size)
	hColor := call(pCreateBitmap, size, size, 1, 32, unsafe.Pointer(&pix[0]))
	hMask := call(pCreateBitmap, size, size, 1, 1, unsafe.Pointer(&mask[0]))
	ii := iconInfo{fIcon: 1, hbmMask: hMask, hbmColor: hColor}
	icon := call(pCreateIconIndirect, unsafe.Pointer(&ii))
	call(pDeleteObject, hColor)
	call(pDeleteObject, hMask)
	if icon == 0 {
		icon = call(pLoadIconW, 0, idiApp)
	}
	return icon
}

// ---------------------------------------------------------------------
// Tray icon

func trayData() notifyIconData {
	var nid notifyIconData
	nid.cbSize = uint32(unsafe.Sizeof(nid))
	nid.hWnd = hMain
	nid.uID = 1
	nid.uFlags = nifMessage | nifIcon | nifTip
	nid.uCallbackMessage = wmTray
	nid.hIcon = hIconSmall
	copy(nid.szTip[:127], syscall.StringToUTF16("Easy Commander host - "+statusText()))
	return nid
}

func trayAdd() {
	nid := trayData()
	call(pShellNotifyIconW, nimAdd, unsafe.Pointer(&nid))
}

func trayUpdate() {
	nid := trayData()
	call(pShellNotifyIconW, nimModify, unsafe.Pointer(&nid))
}

func trayDelete() {
	nid := trayData()
	call(pShellNotifyIconW, nimDelete, unsafe.Pointer(&nid))
}

func trayMenu() {
	menu := call(pCreatePopupMenu)
	toggle := "Start sharing"
	if host.Running() {
		toggle = "Stop sharing"
	}
	call(pAppendMenuW, menu, mfString, idTrayOpen, wstr("Open window"))
	call(pAppendMenuW, menu, mfString, idTrayToggle, wstr(toggle))
	rxToggle := "Start receiving"
	if host.Receiving() {
		rxToggle = "Stop receiving"
	}
	call(pAppendMenuW, menu, mfString, idTrayRx, wstr(rxToggle))
	call(pAppendMenuW, menu, mfSeparator, 0, 0)
	call(pAppendMenuW, menu, mfString, idTrayExit, wstr("Exit"))

	var pt point
	call(pGetCursorPos, unsafe.Pointer(&pt))
	call(pSetForegroundWindow, hMain) // otherwise the menu does not close when clicked elsewhere
	call(pTrackPopupMenu, menu, tpmRightBtn, pt.x, pt.y, 0, hMain, 0)
	call(pPostMessageW, hMain, wmNull, 0, 0)
	call(pDestroyMenu, menu)
}

// ---------------------------------------------------------------------
// Window content

func createControls() {
	control("STATIC", "Shared folder (the DOS client can only read it):", ssNoPrefix, 0, 12, 12, 456, 18, 0)
	hEdFolder = control("EDIT", "", esAutoHScroll|wsTabStop, wsExClientEdge, 12, 32, 372, 24, idFolder)
	call(pSendMessageW, hEdFolder, emSetLimit, 259, 0)
	hBtnBrowse = control("BUTTON", "Browse...", bsPushButton|wsTabStop, 0, 392, 31, 76, 26, idBrowse)

	control("STATIC", "Name shown to DOS clients:", ssNoPrefix, 0, 12, 70, 250, 18, 0)
	hEdName = control("EDIT", "", esAutoHScroll|wsTabStop, wsExClientEdge, 12, 90, 250, 24, idName)
	call(pSendMessageW, hEdName, emSetLimit, 24, 0)
	control("STATIC", "Port:", ssNoPrefix, 0, 280, 70, 80, 18, 0)
	hEdPort = control("EDIT", "", esAutoHScroll|esNumber|wsTabStop, wsExClientEdge, 280, 90, 80, 24, idPort)
	call(pSendMessageW, hEdPort, emSetLimit, 5, 0)

	hBtnStart = control("BUTTON", "Start sharing", bsPushButton|wsTabStop, 0, 12, 128, 140, 30, idStart)
	hLblStatus = control("STATIC", "", ssNoPrefix, 0, 168, 135, 300, 20, 0)

	if !readOnlyBuild { // the read-only build has no receiving at all
		control("STATIC", "Inbox folder (what the DOS client sends arrives here):", ssNoPrefix, 0, 12, 176, 456, 18, 0)
		hEdInbox = control("EDIT", "", esAutoHScroll|wsTabStop, wsExClientEdge, 12, 196, 372, 24, idInFolder)
		call(pSendMessageW, hEdInbox, emSetLimit, 259, 0)
		hBtnInBrowse = control("BUTTON", "Browse...", bsPushButton|wsTabStop, 0, 392, 195, 76, 26, idInBrowse)

		control("STATIC", "Quota (like 500 MB or 4 GB, 0 = no limit):", ssNoPrefix, 0, 12, 232, 250, 18, 0)
		hEdQuota = control("EDIT", "", esAutoHScroll|wsTabStop, wsExClientEdge, 12, 252, 120, 24, idQuota)
		call(pSendMessageW, hEdQuota, emSetLimit, 20, 0)
		hChkAsk = control("BUTTON", "Ask me before every transfer", bsAutoCheckbox|wsTabStop, 0, 160, 254, 250, 22, idAsk)

		hBtnRx = control("BUTTON", "Start receiving", bsPushButton|wsTabStop, 0, 12, 292, 140, 30, idRxStart)
		hLblRx = control("STATIC", "", ssNoPrefix, 0, 168, 299, 300, 20, 0)

	}

	control("STATIC", "Address to type into the DOS client:", ssNoPrefix, 0, 12, 336, 456, 18, 0)
	hLblIPs = control("STATIC", "", ssNoPrefix, 0, 12, 356, 456, 40, 0)

	control("STATIC", "Activity:", ssNoPrefix, 0, 12, 400, 456, 18, 0)
	hList = control("LISTBOX", "", wsVScroll|lbsNoIntegral, wsExClientEdge, 12, 420, 456, 92, 0)

	hChkHide = control("BUTTON", "Start hidden in the tray", bsAutoCheckbox|wsTabStop, 0, 12, 524, 230, 22, idHidden)
	control("BUTTON", "Hide to tray", bsPushButton|wsTabStop, 0, 270, 520, 100, 28, idHide)
	control("BUTTON", "Exit", bsPushButton|wsTabStop, 0, 378, 520, 90, 28, idExit)
}

func statusText() string {
	share := "Stopped"
	if host.Running() {
		n := host.Clients()
		if n == 1 {
			share = "Sharing - 1 client connected"
		} else {
			share = fmt.Sprintf("Sharing - %d clients connected", n)
		}
	}
	if host.Receiving() {
		if host.Running() {
			return share + "; receiving"
		}
		return "Receiving"
	}
	return share
}

func rxStatusText() string {
	switch {
	case !host.Receiving():
		return "Not receiving"
	case host.ReceiveBusy():
		return "Receiving - a transfer is running"
	case host.ReceiveAsks():
		return "Receiving - asks before every transfer"
	}
	return "Receiving - without asking"
}

func ipText() string {
	ips := localIPs()
	if len(ips) == 0 {
		return "(no local network address found)"
	}
	return strings.Join(ips, "\r\n")
}

// refresh brings the buttons, status text and address list up to date.
func refresh() {
	running := host.Running()
	if running {
		setText(hBtnStart, "Stop sharing")
	} else {
		setText(hBtnStart, "Start sharing")
	}
	en := uintptr(1)
	if running {
		en = 0
	}
	call(pEnableWindow, hEdFolder, en)
	call(pEnableWindow, hEdName, en)
	call(pEnableWindow, hEdPort, en)
	call(pEnableWindow, hBtnBrowse, en)

	rx := host.Receiving()
	if rx {
		setText(hBtnRx, "Stop receiving")
	} else {
		setText(hBtnRx, "Start receiving")
	}
	enRx := uintptr(1)
	if rx {
		enRx = 0
	}
	call(pEnableWindow, hEdInbox, enRx)
	call(pEnableWindow, hBtnInBrowse, enRx)
	call(pEnableWindow, hEdQuota, enRx)
	call(pEnableWindow, hChkAsk, enRx)
	// the port and the name belong to the open ports: fixed while either is on
	if running || rx {
		call(pEnableWindow, hEdName, 0)
		call(pEnableWindow, hEdPort, 0)
	}
	if s := rxStatusText(); s != lastRxStatus {
		lastRxStatus = s
		setText(hLblRx, s)
	}

	if s := statusText(); s != lastStatus {
		lastStatus = s
		setText(hLblStatus, s)
		trayUpdate()
	}
	if s := ipText(); s != lastIPs {
		lastIPs = s
		setText(hLblIPs, s)
	}
}

// pumpLog copies new log lines into the list.
func pumpLog() {
	lines, next := ring.since(ringPos)
	ringPos = next
	if len(lines) == 0 {
		return
	}
	for _, l := range lines {
		call(pSendMessageW, hList, lbAddString, 0, wstr(l))
	}
	count := int(call(pSendMessageW, hList, lbGetCount, 0, 0))
	for count > 500 {
		call(pSendMessageW, hList, lbDeleteStr, 0, 0)
		count--
	}
	itemH := int(call(pSendMessageW, hList, lbGetItemH, 0, 0))
	if itemH > 0 {
		rows := (sc(92) - 6) / itemH
		top := count - rows
		if top < 0 {
			top = 0
		}
		call(pSendMessageW, hList, lbSetTopIndex, top, 0)
	}
}

func showWindow() {
	call(pShowWindow, hMain, swShow)
	call(pSetForegroundWindow, hMain)
	refresh()
}

func browseFolder(target uintptr, title string) {
	display := make([]uint16, 260)
	bi := browseInfo{
		hwndOwner:      hMain,
		pszDisplayName: &display[0],
		lpszTitle:      wstr(title),
		ulFlags:        0x0001 | 0x0010 | 0x0040, // folders only, edit box, new style
	}
	pidl := call(pSHBrowseForFolderW, unsafe.Pointer(&bi))
	if pidl == 0 {
		return
	}
	path := make([]uint16, 520)
	if call(pSHGetPathFromIDListW, pidl, unsafe.Pointer(&path[0])) != 0 {
		setText(target, syscall.UTF16ToString(path))
	}
	call(pCoTaskMemFree, pidl)
}

func saveSettings() {
	cfg.Dir = getText(hEdFolder)
	cfg.Name = strings.TrimSpace(getText(hEdName))
	if n, err := strconv.Atoi(strings.TrimSpace(getText(hEdPort))); err == nil && n >= 1 && n <= 65535 {
		cfg.Port = n
	}
	cfg.Hidden = call(pSendMessageW, hChkHide, bmGetCheck, 0, 0) == 1
	cfg.InDir = strings.TrimSpace(getText(hEdInbox))
	if mb, err := parseQuotaText(getText(hEdQuota)); err == nil {
		cfg.QuotaMB = mb
	}
	cfg.Ask = call(pSendMessageW, hChkAsk, bmGetCheck, 0, 0) == 1
	saveConfig(cfgPath, cfg)
}

func toggleHost() {
	if host.Running() {
		host.StopSharing()
		logf("Stopped sharing")
		refresh()
		return
	}
	port, err := strconv.Atoi(strings.TrimSpace(getText(hEdPort)))
	if err != nil || port < 1 || port > 65535 {
		msgBox("The port must be a number from 1 to 65535.", mbOK|mbIconError)
		return
	}
	if host.Receiving() {
		port = host.Port // the ports are open already (for receiving)
	}
	if err := host.Start(getText(hEdFolder), port, strings.TrimSpace(getText(hEdName))); err != nil {
		msgBox(err.Error(), mbOK|mbIconError)
		return
	}
	announceStart()
	saveSettings()
	refresh()
}

// The question about an incoming transfer. It comes from a connection's own
// goroutine, so the box gets a thread of its own, and when the host stops
// waiting (ctx is done) the box is closed by "pressing No" in it.
var closeBoxCallback = syscall.NewCallback(func(hwnd, lparam uintptr) uintptr {
	call(pPostMessageW, hwnd, wmCommand, idNo, 0)
	return 1
})

func guiConfirm(ctx context.Context, info OfferInfo) bool {
	text := fmt.Sprintf("%s wants to send %d files (%s).\n\nThey will go into the folder\n%s\n\n",
		info.Remote, info.Files, humanBytes(info.Bytes), filepath.Join(host.InboxDir, info.Dest))
	if info.Free >= 0 {
		text += "Free disk space: " + humanBytes(info.Free) + "\n"
	}
	text += "In the inbox now: " + humanBytes(info.Used)
	if info.Quota > 0 {
		text += " of the " + humanBytes(info.Quota) + " quota"
	}
	text += "\n\nAccept?"

	done := make(chan bool, 1)
	tid := make(chan uint32, 1)
	go func() {
		runtime.LockOSThread() // the box lives and dies with this thread
		tid <- uint32(call(pGetCurrentThreadID))
		r := call(pMessageBoxW, 0, wstr(text), wstr("Easy Commander host - incoming files"),
			mbYesNo|mbIconQuestion|mbDefButton2|mbTopmost|mbSetForeground)
		done <- r == idYes
	}()
	t := <-tid
	select {
	case yes := <-done:
		return yes
	case <-ctx.Done():
		for i := 0; i < 25; i++ { // the box may not be on the screen yet
			call(pEnumThreadWindows, t, closeBoxCallback, 0)
			select {
			case <-done:
				return false
			case <-time.After(200 * time.Millisecond):
			}
		}
		return false
	}
}

func toggleReceiving() {
	if host.Receiving() {
		host.StopReceiving()
		logf("Stopped receiving")
		refresh()
		return
	}
	dir := strings.TrimSpace(getText(hEdInbox))
	if dir == "" {
		msgBox("Choose the inbox folder first.", mbOK|mbIconError)
		return
	}
	mb, err := parseQuotaText(getText(hEdQuota))
	if err != nil {
		msgBox("The quota must be a size like 500 MB or 4 GB (0 = no limit).", mbOK|mbIconError)
		return
	}
	port, err := strconv.Atoi(strings.TrimSpace(getText(hEdPort)))
	if host.Running() {
		port, err = host.Port, nil
	}
	if err != nil || port < 1 || port > 65535 {
		msgBox("The port must be a number from 1 to 65535.", mbOK|mbIconError)
		return
	}
	ask := call(pSendMessageW, hChkAsk, bmGetCheck, 0, 0) == 1

	if !ask {
		q := "without any limit (only the free disk space)"
		if mb > 0 {
			q = "up to " + quotaText(mb)
		}
		text := "You are about to receive WITHOUT being asked.\n\n" +
			"Every computer in your network can then put files into\n" + dir + "\n" +
			q + ", until you press Stop receiving.\n\n" +
			"Received files are never run, but treat them as untrusted.\n\nStart receiving?"
		if askBox(text, mbYesNo|mbIconWarning|mbDefButton2) != idYes {
			return
		}
	}

	icfg := InboxConfig{Dir: dir, Quota: int64(mb) << 20, Reserve: defaultReserve, Ask: ask}
	if err := host.StartReceiving(port, strings.TrimSpace(getText(hEdName)), icfg, guiConfirm); err != nil {
		msgBox(err.Error(), mbOK|mbIconError)
		return
	}
	logf("Receiving into %s (quota %s, %s)", host.InboxDir, quotaText(mb), map[bool]string{true: "asks before every transfer", false: "without asking"}[ask])
	if host.DiscoveryErr != nil {
		logf("Host search is off (UDP port %d: %v)", host.Port, host.DiscoveryErr)
	}
	saveSettings()
	refresh()
}

func announceStart() {
	logf("Sharing %s on port %d", host.Dir, host.Port)
	if host.DiscoveryErr != nil {
		logf("Host search is off (UDP port %d: %v)", host.Port, host.DiscoveryErr)
	}
}

func exitProgram() {
	saveSettings()
	host.Stop()
	call(pDestroyWindow, hMain)
}

func onCommand(id int) {
	switch id {
	case idBrowse:
		browseFolder(hEdFolder, "Choose the folder to share")
	case idInBrowse:
		browseFolder(hEdInbox, "Choose the inbox folder")
	case idStart, idTrayToggle:
		toggleHost()
	case idRxStart, idTrayRx:
		toggleReceiving()
	case idAsk:
		saveSettings()
	case idHidden:
		saveSettings()
	case idHide:
		call(pShowWindow, hMain, swHide)
	case idExit, idTrayExit:
		exitProgram()
	case idTrayOpen:
		showWindow()
	}
}

func wndProc(hwnd, msg, wParam, lParam uintptr) uintptr {
	if msgTaskbarCreated != 0 && msg == msgTaskbarCreated {
		trayAdd() // Explorer was restarted: put the icon back
		return 0
	}
	switch msg {
	case wmCreate:
		hMain = hwnd
		createControls()
		return 0
	case wmCommand:
		onCommand(int(wParam & 0xFFFF))
		return 0
	case wmTimer:
		pumpLog()
		refresh()
		return 0
	case wmTray:
		switch lParam {
		case wmLButtonUp, wmLButtonDbl:
			showWindow()
		case wmRButtonUp:
			trayMenu()
		}
		return 0
	case wmShowMe:
		showWindow()
		return 0
	case wmClose:
		call(pShowWindow, hwnd, swHide) // the program keeps running in the tray
		return 0
	case wmDestroy:
		call(pKillTimer, hwnd, timerID)
		trayDelete()
		call(pPostQuitMessage, 0)
		return 0
	}
	return call(pDefWindowProcW, hwnd, msg, wParam, lParam)
}

// ---------------------------------------------------------------------
// Start

// attachConsole lets -console print into the console it was started from
// (a program built with -H=windowsgui has none of its own).
func attachConsole() {
	if call(pAttachConsole, uint32(0xFFFFFFFF)) == 0 {
		return
	}
	if f, err := os.OpenFile("CONOUT$", os.O_WRONLY, 0); err == nil {
		os.Stdout, os.Stderr = f, f
		log.SetOutput(f)
	}
	if f, err := os.OpenFile("CONIN$", os.O_RDONLY, 0); err == nil {
		os.Stdin = f
	}
}

func runGUI(o options) {
	runtime.LockOSThread() // the window and its message loop must stay on one thread
	call(pFreeConsole)

	// Only one copy: a second start brings the first window to the front.
	mutexName := wstr("Local\\EasyCommanderHostMutex")
	_, _, errno := pCreateMutexW.Call(0, 0, uintptr(unsafe.Pointer(mutexName)))
	runtime.KeepAlive(mutexName)
	if errno == syscall.Errno(183) { // ERROR_ALREADY_EXISTS
		if w := call(pFindWindowW, wstr(windowClass), 0); w != 0 {
			call(pPostMessageW, w, wmShowMe, 0, 0)
		}
		return
	}

	// settings: saved file, then the command line
	exe, _ := os.Executable()
	exeDir := filepath.Dir(exe)
	cfgPath = filepath.Join(exeDir, "echost.ini")
	cfg = loadConfig(cfgPath)
	if o.dirSet {
		cfg.Dir = o.dir
	}
	if o.portSet {
		cfg.Port = o.port
	}
	if o.nameSet {
		cfg.Name = o.name
	}
	if cfg.Dir == "" {
		d := filepath.Join(exeDir, "share")
		if os.MkdirAll(d, 0o755) == nil {
			cfg.Dir = d
		}
	}

	logf = func(format string, args ...any) {
		ring.add(time.Now().Format("15:04:05") + "  " + fmt.Sprintf(format, args...))
	}

	enableVisualStyles()
	call(pSetProcessDPIAware)
	if dc := call(pGetDC, 0); dc != 0 {
		if d := int(call(pGetDeviceCaps, dc, 88)); d > 0 { // LOGPIXELSX
			dpi = d
		}
		call(pReleaseDC, 0, dc)
	}
	call(pOleInitialize, 0) // the folder chooser needs it
	msgTaskbarCreated = call(pRegisterWindowMessageW, wstr("TaskbarCreated"))

	hInst = call(pGetModuleHandleW, 0)
	hFont = call(pCreateFontW, -sc(12), 0, 0, 0, 400, 0, 0, 0, 1, 0, 0, 5, 0, wstr("Segoe UI"))
	hIconBig = makeIcon(sc(32))
	hIconSmall = makeIcon(int(call(pGetSystemMetrics, 49))) // SM_CXSMICON

	wc := wndClassEx{
		lpfnWndProc:   syscall.NewCallback(wndProc),
		hInstance:     hInst,
		hIcon:         hIconBig,
		hCursor:       call(pLoadCursorW, 0, idcArrow),
		hbrBackground: colorBtnFace + 1, // the usual grey of dialog windows
		lpszClassName: wstr(windowClass),
		hIconSm:       hIconSmall,
	}
	wc.cbSize = uint32(unsafe.Sizeof(wc))
	if call(pRegisterClassExW, unsafe.Pointer(&wc)) == 0 {
		return
	}

	style := uint32(wsOverlapped | wsCaption | wsSysMenu | wsMinimizeBox)
	r := rect{0, 0, int32(sc(480)), int32(sc(560))}
	call(pAdjustWindowRectEx, unsafe.Pointer(&r), style, 0, 0)
	w, h := int(r.right-r.left), int(r.bottom-r.top)
	x := (int(call(pGetSystemMetrics, 0)) - w) / 2
	y := (int(call(pGetSystemMetrics, 1)) - h) / 2

	hwnd := call(pCreateWindowExW, 0, wstr(windowClass), wstr(windowTitle), style,
		x, y, w, h, 0, 0, hInst, 0)
	if hwnd == 0 {
		return
	}
	hMain = hwnd

	setText(hEdFolder, cfg.Dir)
	if cfg.Name != "" {
		setText(hEdName, cfg.Name)
	} else if hn, err := os.Hostname(); err == nil {
		setText(hEdName, sanitizeName(hn)) // what clients see unless it is changed
	}
	setText(hEdPort, strconv.Itoa(cfg.Port))
	if cfg.InDir == "" {
		cfg.InDir = filepath.Join(exeDir, "inbox") // only a suggestion: nothing is created before Start receiving
	}
	setText(hEdInbox, cfg.InDir)
	setText(hEdQuota, quotaText(cfg.QuotaMB))
	if cfg.Ask {
		call(pSendMessageW, hChkAsk, bmSetCheck, 1, 0)
	}
	if cfg.Hidden {
		call(pSendMessageW, hChkHide, bmSetCheck, 1, 0)
	}

	trayAdd()
	call(pSetTimer, hMain, timerID, 1000, 0)

	// Start sharing at once, as the console version does.
	started := false
	var startErr error
	if startErr = host.Start(cfg.Dir, cfg.Port, cfg.Name); startErr == nil {
		started = true
		announceStart()
	} else {
		logf("Not sharing: %v", startErr)
	}
	pumpLog()
	refresh()

	if !(cfg.Hidden && started) {
		showWindow()
	}
	if startErr != nil {
		msgBox(startErr.Error(), mbOK|mbIconError)
	}

	var m msgT
	for {
		r := int32(call(pGetMessageW, unsafe.Pointer(&m), 0, 0, 0))
		if r <= 0 { // 0: quit, -1: error
			break
		}
		if call(pIsDialogMessageW, hMain, unsafe.Pointer(&m)) != 0 {
			continue // Tab moves between the controls
		}
		call(pTranslateMessage, unsafe.Pointer(&m))
		call(pDispatchMessageW, unsafe.Pointer(&m))
	}
}
