// Exercise the actual dialog and session window handlers without real credentials or network.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define wWinMain helper_entry_unused
#include "windows_window.c"
#undef wWinMain
#include <assert.h>
static HANDLE gate;
static LONG attempts;
static uint16_t attemptedPort;
static int scenario;
static unsigned cancelReceives,cancelSends;
void clipboard_cancel_files(void) {cancelReceives++;}
void clipboard_cancel_send(void) {cancelSends++;}
int probe_connect(const char *h,uint16_t port,const char *d,const char *u,const char *p) {
    (void)h;(void)d;(void)u;(void)p;InterlockedIncrement(&attempts);
    attemptedPort=port;
    assert(WaitForSingleObject(gate,5000)==WAIT_OBJECT_0);
    if(scenario==1)return 0x00020014;
    BYTE frame[16]={0};probe_frame(frame,2,2,8);
    while(!probe_requested_stop())Sleep(1);
    return 0;
}
// The private station is not visible; inspect requested window visibility instead.
static BOOL shown(HWND w){return IsWindow(w) && (GetWindowLongPtrW(w,GWL_STYLE)&WS_VISIBLE)!=0;}
static RECT bounds(HWND window){RECT result;assert(GetWindowRect(window,&result));return result;}
static void assert_login_columns(HWND dlg) {
    const int labelIds[]={IDC_ACCOUNT_LABEL,IDC_PASSWORD_LABEL,IDC_PORT_LABEL};
    const int fieldIds[]={IDC_ACCOUNT,IDC_PASSWORD,IDC_PORT};
    RECT first=bounds(GetDlgItem(dlg,IDC_ACCOUNT));
    RECT original={96,0,214,0};assert(MapDialogRect(dlg,&original));
    POINT dialogOrigin={0};assert(ClientToScreen(dlg,&dialogOrigin));
    assert(first.left<dialogOrigin.x+original.left && first.right==dialogOrigin.x+original.right);
    HDC dc=GetDC(dlg);assert(dc);
    int textRight=0;
    for(unsigned i=0;i<ARRAYSIZE(labelIds);i++){
        HWND label=GetDlgItem(dlg,labelIds[i]);
        RECT labelRect=bounds(label), fieldRect=bounds(GetDlgItem(dlg,fieldIds[i]));
        assert(fieldRect.left==first.left && fieldRect.right==first.right);
        assert(labelRect.right<fieldRect.left);
        assert(abs((labelRect.top+labelRect.bottom)-(fieldRect.top+fieldRect.bottom))<=8);
        wchar_t text[64];assert(GetWindowTextW(label,text,ARRAYSIZE(text))>0);
        HGDIOBJ font=SelectObject(dc,(HFONT)SendMessageW(label,WM_GETFONT,0,0));
        RECT size={0};assert(DrawTextW(dc,text,-1,&size,DT_CALCRECT|DT_SINGLELINE|DT_NOPREFIX)>0);
        SelectObject(dc,font);
        int right=labelRect.left+size.right;
        assert(right<=labelRect.right);
        textRight=max(textRight,right);
    }
    ReleaseDC(dlg,dc);
    RECT gap={0,0,8,0};assert(MapDialogRect(dlg,&gap));
    assert(first.left-textRight==gap.right);
    const RdpMessage captions[]={Rdp_Connect,Rdp_Connecting,Rdp_Cancel};
    dc=GetDC(dlg);assert(dc);
    for(unsigned i=0;i<ARRAYSIZE(captions);i++){
        HWND button=GetDlgItem(dlg,i==2?IDCANCEL:IDOK);
        HGDIOBJ font=SelectObject(dc,(HFONT)SendMessageW(button,WM_GETFONT,0,0));
        RECT size={0},buttonRect=bounds(button);
        DrawTextW(dc,rdp_text(captions[i]),-1,&size,DT_CALCRECT|DT_SINGLELINE|DT_NOPREFIX);
        assert(size.right<buttonRect.right-buttonRect.left);
        SelectObject(dc,font);
    }
    ReleaseDC(dlg,dc);
}
static void assert_login_surface(HWND dlg) {
    RECT bounds;assert(GetClientRect(dlg,&bounds));
    HRGN region=CreateRectRgn(0,0,0,0);assert(region);
    assert(GetWindowRgn(dlg,region)!=ERROR);
    assert(!PtInRegion(region,0,0));
    assert(PtInRegion(region,bounds.right/2,bounds.bottom-MulDiv(2,GetDpiForWindow(dlg),96)-2));
    DeleteObject(region);
    HDC screen=GetDC(dlg),dc=CreateCompatibleDC(screen);assert(screen && dc);
    HBITMAP bitmap=CreateCompatibleBitmap(screen,bounds.right,bounds.bottom);assert(bitmap);
    HGDIOBJ old=SelectObject(dc,bitmap);
    paint_login_surface(dc,dlg);
    COLORREF top=GetPixel(dc,10,10),other=GetPixel(dc,bounds.right-10,10),bottom=GetPixel(dc,10,bounds.bottom-4);
    assert(GetRValue(top)<80 && GetBValue(top)<130);
    assert(top!=other && GetRValue(bottom)>220 && GetGValue(bottom)>220);
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(dlg,screen);
}
static void assert_login_frame(HWND dlg) {
    LoginFrame *frame=(LoginFrame*)GetPropW(dlg,LOGIN_FRAME_PROP);assert(frame && IsWindow(frame->hwnd) && frame->painted);
    assert(GetWindowLongPtrW(frame->hwnd,GWL_EXSTYLE)&WS_EX_NOACTIVATE);
    assert(GetWindowLongPtrW(frame->hwnd,GWL_EXSTYLE)&WS_EX_TRANSPARENT);
    RECT r=bounds(dlg),shadow=bounds(frame->hwnd);
    int w=r.right-r.left,h=r.bottom-r.top,pad=MulDiv(26,GetDpiForWindow(dlg),96);
    assert(shadow.left==r.left-pad && shadow.top==r.top-pad && shadow.right==r.right+pad && shadow.bottom==r.bottom+pad);
    DWORD *pixels=NULL;HBITMAP bitmap=login_frame_bitmap(dlg,w,h,pad,TRUE,&pixels);assert(bitmap);
    int stride=w+pad*2,offset=max(3,MulDiv(7,GetDpiForWindow(dlg),96));
    DWORD above=pixels[(size_t)(pad-offset)*stride+pad+w/2]>>24;
    DWORD below=pixels[(size_t)(pad+h+offset)*stride+pad+w/2]>>24;
    assert(below>above && below<100);
    assert((pixels[(size_t)(pad+h/2)*stride+pad+w/2]>>24)==255);
    for(int y=0;y<h+pad*2;y++)for(int x=0;x<stride;x++){
        DWORD p=pixels[(size_t)y*stride+x],alpha=p>>24;
        assert((p&255)<=alpha && ((p>>8)&255)<=alpha && ((p>>16)&255)<=alpha);
    }
    DeleteObject(bitmap);
    HWND shadowWindow=frame->hwnd;
    SetWindowPos(dlg,NULL,r.left+10,r.top+15,0,0,SWP_NOZORDER|SWP_NOSIZE|SWP_NOACTIVATE);
    shadow=bounds(shadowWindow);assert(shadow.left==r.left+10-pad && shadow.top==r.top+15-pad);
    SetWindowPos(dlg,NULL,r.left,r.top,0,0,SWP_NOZORDER|SWP_NOSIZE|SWP_NOACTIVATE);
    ShowWindow(dlg,SW_SHOWNOACTIVATE);assert(shown(shadowWindow));
    ShowWindow(dlg,SW_HIDE);assert(!shown(shadowWindow));
}
// Static controls can receive a parent/shared DC. A child background must not
// erase siblings, and WM_PRINT may already have translated the viewport.
static void assert_child_background_clipped(HWND dlg) {
    RECT r;assert(GetClientRect(dlg,&r));
    HDC screen=GetDC(dlg),dc=CreateCompatibleDC(screen);
    HBITMAP bitmap=CreateCompatibleBitmap(screen,r.right,r.bottom);
    HGDIOBJ old=SelectObject(dc,bitmap);
    const int ids[]={IDC_BRAND,IDC_DEVICE,IDC_ACCOUNT_LABEL,IDC_FILES,IDOK,IDC_DISMISS};
    for(unsigned i=0;i<ARRAYSIZE(ids);i++){
        SetDCBrushColor(dc,RGB(255,0,255));FillRect(dc,&r,GetStockObject(DC_BRUSH));
        HWND child=GetDlgItem(dlg,ids[i]);RECT childRect;GetClientRect(child,&childRect);
        POINT origin={0};MapWindowPoints(child,dlg,&origin,1);
        SetViewportOrgEx(dc,origin.x,origin.y,NULL);
        paint_login_parent(dc,dlg,child);
        POINT actual;GetViewportOrgEx(dc,&actual);assert(actual.x==origin.x && actual.y==origin.y);
        SetViewportOrgEx(dc,0,0,NULL);
        assert(GetPixel(dc,0,r.bottom-1)==RGB(255,0,255));
        assert(GetPixel(dc,0,0)==RGB(255,0,255));
        assert(GetPixel(dc,origin.x+childRect.right/2,origin.y+childRect.bottom/2)!=RGB(255,0,255));
    }
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(dlg,screen);
}
static void save_login_screenshot(HWND dlg) {
    wchar_t path[1024];
    DWORD length=GetEnvironmentVariableW(L"CALABI_LOGIN_TEST_SCREENSHOT",path,ARRAYSIZE(path));
    if(!length || length>=ARRAYSIZE(path))return;
    RECT r;assert(GetClientRect(dlg,&r));
    int pad=MulDiv(26,GetDpiForWindow(dlg),96),w=r.right+pad*2,h=r.bottom+pad*2;
    HDC screen=GetDC(dlg),dc=CreateCompatibleDC(screen);assert(screen && dc);
    BITMAPINFO info={0};
    info.bmiHeader=(BITMAPINFOHEADER){sizeof(BITMAPINFOHEADER),w,-h,1,32,BI_RGB};
    void *pixels=NULL;HBITMAP bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,NULL,0);assert(bitmap && pixels);
    HGDIOBJ old=SelectObject(dc,bitmap);
    DWORD *decoration=NULL;HBITMAP backdrop=login_frame_bitmap(dlg,r.right,r.bottom,pad,TRUE,&decoration);assert(backdrop);
    for(size_t i=0;i<(size_t)w*h;i++){
        DWORD p=decoration[i],a=p>>24;
        ((DWORD*)pixels)[i]=(((p>>16&255)+230*(255-a)/255)<<16)|(((p>>8&255)+235*(255-a)/255)<<8)|((p&255)+241*(255-a)/255);
    }
    DeleteObject(backdrop);
    HRGN region=CreateRectRgn(0,0,0,0);assert(region && GetWindowRgn(dlg,region)!=ERROR);
    OffsetRgn(region,pad,pad);SelectClipRgn(dc,region);SetViewportOrgEx(dc,pad,pad,NULL);
    SendMessageW(dlg,WM_PRINT,(WPARAM)dc,PRF_CLIENT|PRF_CHILDREN|PRF_ERASEBKGND);
    GdiFlush();
    DWORD size=(DWORD)((size_t)w*h*4),written;
    BITMAPFILEHEADER header={0};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(info.bmiHeader);header.bfSize=header.bfOffBits+size;
    HANDLE file=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);assert(file!=INVALID_HANDLE_VALUE);
    assert(WriteFile(file,&header,sizeof(header),&written,NULL));
    assert(WriteFile(file,&info.bmiHeader,sizeof(info.bmiHeader),&written,NULL));
    assert(WriteFile(file,pixels,size,&written,NULL));CloseHandle(file);
    SelectObject(dc,old);DeleteObject(region);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(dlg,screen);
}
static void pump(void) {
    MSG m;while(PeekMessageW(&m,NULL,0,0,PM_REMOVE)) {
        if(m.message!=WM_QUIT){TranslateMessage(&m);DispatchMessageW(&m);}
    }
}
static void wait_idle(void) {
    ULONGLONG until=GetTickCount64()+3000;
    while(connecting && GetTickCount64()<until){pump();Sleep(1);}pump();assert(!connecting);
}
static void create_form(void) {
    InterlockedExchange(&closing,0);connecting=FALSE;
    window=CreateWindowExW(0,L"LoginSessionFixture",L"hidden session",WS_OVERLAPPEDWINDOW,0,0,500,400,NULL,NULL,GetModuleHandleW(NULL),NULL);assert(window);
    loginWindow=CreateDialogParamW(GetModuleHandleW(NULL),MAKEINTRESOURCEW(IDD_LOGIN),NULL,login_proc,(LPARAM)L"100.64.0.36");assert(loginWindow);
    SetWindowPos(loginWindow,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);SetDlgItemTextW(loginWindow,IDC_ACCOUNT,L"fixture-user");SetDlgItemTextW(loginWindow,IDC_PASSWORD,L"fake-password");
}
static void test_localized_login(BOOL ssh) {
    sshLogin=ssh;
    for(int i=0;i<RdpLocaleCount;i++){
        SetEnvironmentVariableA("CALABI_UI_LOCALE",rdp_locale_codes[i]);
        HWND localized=CreateDialogParamW(GetModuleHandleW(NULL),MAKEINTRESOURCEW(IDD_LOGIN),NULL,login_proc,(LPARAM)L"127.0.0.1");
        assert(localized);
        wchar_t caption[200];
        GetDlgItemTextW(localized,IDC_BRAND,caption,ARRAYSIZE(caption));
        assert(!wcscmp(caption,rdp_text_in((RdpLocale)i,Rdp_Login)));
        GetDlgItemTextW(localized,IDC_ACCOUNT_LABEL,caption,ARRAYSIZE(caption));
        assert(!wcscmp(caption,rdp_text_in((RdpLocale)i,Rdp_Account)));
        GetDlgItemTextW(localized,IDC_PASSWORD_LABEL,caption,ARRAYSIZE(caption));
        assert(!wcscmp(caption,rdp_text_in((RdpLocale)i,Rdp_Password)));
        GetDlgItemTextW(localized,IDC_PORT_LABEL,caption,ARRAYSIZE(caption));
        assert(!wcscmp(caption,rdp_text_in((RdpLocale)i,Rdp_Port)));
        GetDlgItemTextW(localized,IDC_FILES,caption,ARRAYSIZE(caption));
        assert(!wcscmp(caption,rdp_text_in((RdpLocale)i,Rdp_AllowFiles)));
        assert(shown(GetDlgItem(localized,IDC_PORT)) && shown(GetDlgItem(localized,IDC_PORT_LABEL)));
        assert(login_port(localized)==(ssh?22:PROBE_RDP_DEFAULT_PORT));
        assert_login_columns(localized);
        assert_login_surface(localized);
        assert_login_frame(localized);
        assert_child_background_clipped(localized);
        if(!ssh && !strcmp(rdp_locale_codes[i],"zh-CN")){
            SetDlgItemTextW(localized,IDC_ACCOUNT,L"waiwofei");
            save_login_screenshot(localized);
        }
        assert(!shown(GetDlgItem(localized,IDC_HINT)));
        HWND frame=((LoginFrame*)GetPropW(localized,LOGIN_FRAME_PROP))->hwnd;
        DestroyWindow(localized);assert(!IsWindow(frame));
    }
}
int main(void) {
    SetProcessDPIAware(); // Match the actual helper before creating any dialog.
    SetEnvironmentVariableW(L"CALABI_UI_LOCALE_FILE",NULL);
    SetEnvironmentVariableW(L"CALABI_RDP_PORT",NULL);
    SetEnvironmentVariableW(L"CALABI_SSH_PORT",NULL);
    HWINSTA station=CreateWindowStationW(NULL,0,WINSTA_ALL_ACCESS,NULL);assert(station && SetProcessWindowStation(station));
    HDESK desk=CreateDesktopW(L"Default",NULL,NULL,0,GENERIC_ALL,NULL);assert(desk && SetThreadDesktop(desk));
    panelBrush=CreateSolidBrush(panel);fieldBrush=CreateSolidBrush(field);
    test_localized_login(FALSE);
    test_localized_login(TRUE);
    SetEnvironmentVariableW(L"CALABI_UI_LOCALE",L"zh-CN");
    SetEnvironmentVariableW(L"CALABI_SSH_PORT",L"2222");
    SetEnvironmentVariableW(L"CALABI_RDP_ACCOUNT_HINT",L"fixture-ssh");
    SetEnvironmentVariableW(L"CALABI_RDP_CREDENTIAL_KEY",L"0000000000000000000000000000000000000000000000000000000000000000");
    sshLogin=TRUE;
    loginWindow=CreateDialogParamW(GetModuleHandleW(NULL),MAKEINTRESOURCEW(IDD_LOGIN),NULL,login_proc,(LPARAM)L"100.64.0.36");
    assert(loginWindow && !shown(loginWindow) && login_port(loginWindow)==2222);
    assert(shown(GetDlgItem(loginWindow,IDC_PORT)) && shown(GetDlgItem(loginWindow,IDC_PORT_LABEL)));
    assert(!shown(GetDlgItem(loginWindow,IDC_FILES)));
    assert(IsDlgButtonChecked(loginWindow,IDC_REMEMBER)==BST_UNCHECKED);
    HWND sshHint=GetDlgItem(loginWindow,IDC_HINT),sshRemember=GetDlgItem(loginWindow,IDC_REMEMBER);
    RECT sshCompact=bounds(loginWindow);
    assert(!shown(sshHint) && bounds(sshRemember).top<bounds(sshHint).bottom);
    wchar_t sshText[200];
    GetDlgItemTextW(loginWindow,IDC_ACCOUNT_LABEL,sshText,ARRAYSIZE(sshText));
    assert(!wcscmp(sshText,L"账号"));
    GetDlgItemTextW(loginWindow,IDC_PASSWORD_LABEL,sshText,ARRAYSIZE(sshText));
    assert(!wcscmp(sshText,L"密码"));
    GetDlgItemTextW(loginWindow,IDC_PORT_LABEL,sshText,ARRAYSIZE(sshText));
    assert(!wcscmp(sshText,L"端口"));
    GetDlgItemTextW(loginWindow,IDC_ACCOUNT,sshText,ARRAYSIZE(sshText));
    assert(!wcscmp(sshText,L"fixture-ssh"));
    connecting=TRUE;enable_login(FALSE);
    SendMessageW(loginWindow,MSG_SSH_RESULT,SSH_AUTH,0);
    assert(shown(sshHint) && bounds(sshRemember).top>bounds(sshHint).bottom);
    assert(bounds(loginWindow).top==sshCompact.top && bounds(loginWindow).bottom>sshCompact.bottom);
    GetDlgItemTextW(loginWindow,IDC_HINT,sshText,ARRAYSIZE(sshText));
    assert(!wcscmp(sshText,rdp_text(Rdp_SshAuthFailed)));
    assert(!connecting && IsWindowEnabled(GetDlgItem(loginWindow,IDOK)));
    DestroyWindow(loginWindow);loginWindow=NULL;sshLogin=FALSE;
    SetEnvironmentVariableW(L"CALABI_SSH_PORT",NULL);
    SetEnvironmentVariableW(L"CALABI_RDP_ACCOUNT_HINT",NULL);
    SetEnvironmentVariableW(L"CALABI_RDP_CREDENTIAL_KEY",NULL);
    rdpLoginParent=GetCurrentProcessId();
    WNDCLASSW wc={0};wc.lpfnWndProc=proc;wc.hInstance=GetModuleHandleW(NULL);wc.lpszClassName=L"LoginSessionFixture";assert(RegisterClassW(&wc));
    gate=CreateEventW(NULL,TRUE,FALSE,NULL);assert(gate);scenario=1;create_form();
    HWND remember=GetDlgItem(loginWindow,IDC_REMEMBER);
    assert(SendMessageW(remember,BM_GETCHECK,0,0)==BST_UNCHECKED);
    HDC checkboxDC=CreateCompatibleDC(NULL);assert(checkboxDC);
    NMCUSTOMDRAW checkboxDraw={0};
    checkboxDraw.hdr.hwndFrom=remember;checkboxDraw.hdr.idFrom=IDC_REMEMBER;checkboxDraw.hdr.code=NM_CUSTOMDRAW;
    checkboxDraw.dwDrawStage=CDDS_PREPAINT;checkboxDraw.hdc=checkboxDC;GetClientRect(remember,&checkboxDraw.rc);
    SendMessageW(loginWindow,WM_NOTIFY,IDC_REMEMBER,(LPARAM)&checkboxDraw);
    assert(GetWindowLongPtrW(loginWindow,DWLP_MSGRESULT)==CDRF_SKIPDEFAULT);
    assert(GetTextColor(checkboxDC)==RGB(78,94,115));
    DeleteDC(checkboxDC);
    assert(!shown(window));
    HWND rdpHint=GetDlgItem(loginWindow,IDC_HINT),rdpFiles=GetDlgItem(loginWindow,IDC_FILES);
    RECT rdpCompact=bounds(loginWindow);
    assert(!shown(rdpHint) && bounds(rdpFiles).top<bounds(rdpHint).bottom);
    SetDlgItemTextW(loginWindow,IDC_PORT,L"65536");
    SendMessageW(loginWindow,WM_COMMAND,IDOK,0);
    assert(!connecting && !attempts);
    RECT rdpExpanded=bounds(loginWindow);
    assert(shown(rdpHint) && bounds(rdpFiles).top>bounds(rdpHint).bottom);
    assert(rdpExpanded.top==rdpCompact.top && rdpExpanded.bottom>rdpCompact.bottom);
    assert_login_surface(loginWindow);
    SetDlgItemInt(loginWindow,IDC_PORT,3391,FALSE);
    CheckDlgButton(loginWindow,IDC_FILES,BST_UNCHECKED);
    SendMessageW(loginWindow,WM_COMMAND,IDOK,0);
    assert(connecting && shown(loginWindow) && !shown(window));
    assert(bounds(loginWindow).bottom==rdpExpanded.bottom);
    assert(probe_clipboard_enabled() && probe_clipboard_content_enabled() && !probe_clipboard_files_enabled());
    SendMessageW(loginWindow,WM_COMMAND,IDOK,0);SetEvent(gate);wait_idle();assert(attempts==1);
    assert(shown(loginWindow) && !shown(window));
    wchar_t value[200];GetDlgItemTextW(loginWindow,IDC_ACCOUNT,value,ARRAYSIZE(value));assert(!wcscmp(value,L"fixture-user"));
    assert(!GetDlgItemTextW(loginWindow,IDC_PASSWORD,value,ARRAYSIZE(value)));
    GetDlgItemTextW(loginWindow,IDC_HINT,value,ARRAYSIZE(value));assert(wcsstr(value,L"帐号和密码"));
    assert(IsWindowEnabled(GetDlgItem(loginWindow,IDOK)));
    scenario=2;ResetEvent(gate);SetDlgItemTextW(loginWindow,IDC_PASSWORD,L"retry-password");
    CheckDlgButton(loginWindow,IDC_FILES,BST_CHECKED);
    SendMessageW(loginWindow,WM_COMMAND,IDOK,0);assert(!shown(window));SetEvent(gate);
    ULONGLONG until=GetTickCount64()+3000;while(loginWindow && GetTickCount64()<until){pump();Sleep(1);}
    assert(!loginWindow && shown(window) && ready && attempts==2 && attemptedPort==3391 && fullscreen);
    enqueue(PROBE_SCAN,0x1c,0,0);assert(count==1);
    probe_reconnecting(1);pump();
    assert(shown(window) && fullscreen && !ready && reconnectAttempt==1 && count==0);
    enqueue(PROBE_SCAN,0x1c,0,0);assert(count==0);
    BYTE recovered[16]={0};probe_frame(recovered,2,2,8);pump();
    assert(shown(window) && fullscreen && ready && reconnectAttempt==0);
    probe_files_progress(2,4,3);pump();RECT card;GetClientRect(fileStatusWindow,&card);
    SendMessageW(fileStatusWindow,WM_LBUTTONDOWN,0,MAKELPARAM(card.right-20,20));assert(!cancelSends && !cancelReceives && !shown(fileStatusWindow));
    probe_files_progress(3,4,3);pump();assert(!shown(fileStatusWindow));
    probe_files_progress(4,4,4);pump();assert(shown(fileStatusWindow));
    probe_files_progress(2,4,1);pump();SendMessageW(fileStatusWindow,WM_LBUTTONDOWN,0,MAKELPARAM(card.right-20,20));assert(cancelReceives==1);
    assert(fullscreen);RECT client;GetClientRect(window,&client);
    toolbar_pointer((POINT){client.right/2,0},TRUE,100);assert(shown(sessionToolbar));
    toolbar_pointer((POINT){0,200},TRUE,1400);assert(!shown(sessionToolbar));
    toolbar_pointer((POINT){client.right/2,0},TRUE,1500);assert(shown(sessionToolbar));
    SendMessageW(sessionToolbar,WM_COMMAND,TOOL_MINIMIZE,0);assert(IsIconic(window));ShowWindow(window,SW_RESTORE);
    SendMessageW(sessionToolbar,WM_COMMAND,TOOL_EXIT_FULLSCREEN,0);assert(!fullscreen && !shown(sessionToolbar));
    assert(count==0); // Toolbar actions must not become remote mouse/keyboard input.
    probe_reconnecting(2);pump();assert(shown(window) && !ready);
    toggle(window);GetClientRect(window,&client);toolbar_pointer((POINT){client.right/2,0},TRUE,1600);
    SendMessageW(sessionToolbar,WM_COMMAND,TOOL_END_SESSION,0);wait_idle();assert(!IsWindow(window));
    scenario=3;ResetEvent(gate);create_form();SendMessageW(loginWindow,WM_COMMAND,IDOK,0);
    SendMessageW(loginWindow,WM_CLOSE,0,0);assert(!loginWindow && !shown(window));
    SetEvent(gate);wait_idle();assert(!IsWindow(window));
    puts("Windows UI passed: login retry/cancel, reconnect input gating and recovery, transfer cancellation, fullscreen toolbar and closing while reconnecting.");
    return 0;
}
