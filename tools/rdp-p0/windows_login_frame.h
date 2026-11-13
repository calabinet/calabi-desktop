// A noninteractive alpha backplate keeps the native dialog and its Edit controls
// opaque, while providing an antialiased outline and a soft shadow on Win10/11.
#define LOGIN_FRAME_PROP L"Calabi.LoginFrame"
typedef struct {
    HWND hwnd;
    int width,height,dpi;
    BOOL active,painted;
} LoginFrame;

static double login_round_distance(double x,double y,double w,double h,double r) {
    double dx=fabs(x-w/2)-(w/2-r),dy=fabs(y-h/2)-(h/2-r);
    return hypot(fmax(dx,0),fmax(dy,0))+fmin(fmax(dx,dy),0)-r;
}
static double login_coverage(double distance) {return fmax(0,fmin(1,0.5-distance));}
static COLORREF login_edge_fill(int x,int y,int w,int h,int header) {
    BOOL top=y<header;
    COLORREF a=top?RGB(34,60,91):RGB(248,251,255),b=top?RGB(23,40,62):RGB(240,245,251);
    double t=0.65*(double)x/max(1,w-1)+0.35*(double)y/max(1,(top?header:h)-1);
    return RGB((int)(GetRValue(a)+t*((int)GetRValue(b)-GetRValue(a))),
               (int)(GetGValue(a)+t*((int)GetGValue(b)-GetGValue(a))),
               (int)(GetBValue(a)+t*((int)GetBValue(b)-GetBValue(a))));
}
static HBITMAP login_frame_bitmap(HWND dlg,int w,int h,int pad,BOOL active,DWORD **data) {
    int dpi=GetDpiForWindow(dlg),bw=w+pad*2,bh=h+pad*2;
    double scale=dpi/96.0,radius=18*scale,blur=7*scale,offset=4*scale;
    RECT header={0,0,0,57};MapDialogRect(dlg,&header);
    BITMAPINFO info={0};info.bmiHeader=(BITMAPINFOHEADER){sizeof(BITMAPINFOHEADER),bw,-bh,1,32,BI_RGB};
    HBITMAP bitmap=CreateDIBSection(NULL,&info,DIB_RGB_COLORS,(void**)data,NULL,0);
    if(!bitmap)return NULL;
    for(int y=0;y<bh;y++)for(int x=0;x<bw;x++){
        double px=x-pad+0.5,py=y-pad+0.5;
        double d=login_round_distance(px,py,w,h,radius),cover=login_coverage(d);
        double sd=fmax(0,login_round_distance(px,py-offset,w,h,radius));
        double shadow=(active?0.24:0.16)*exp(-sd*sd/(2*blur*blur));
        double alpha=cover+shadow*(1-cover);
        COLORREF fill=login_edge_fill(max(0,min(w-1,x-pad)),max(0,min(h-1,y-pad)),w,h,header.bottom);
        // A quiet highlight across the dark header, a firmer lower rim beneath
        // the light body; the line is one physical pixel, not a heavy frame.
        double rim=cover>0?fmin(1,login_coverage(d+1.0)/cover):0;
        COLORREF edge=py<header.bottom?RGB(100,125,156):RGB(171,187,207);
        double red=(GetRValue(fill)*rim+GetRValue(edge)*(1-rim))*cover+15*shadow*(1-cover);
        double green=(GetGValue(fill)*rim+GetGValue(edge)*(1-rim))*cover+27*shadow*(1-cover);
        double blue=(GetBValue(fill)*rim+GetBValue(edge)*(1-rim))*cover+45*shadow*(1-cover);
        (*data)[(size_t)y*bw+x]=((DWORD)lround(alpha*255)<<24)|((DWORD)lround(red)<<16)|((DWORD)lround(green)<<8)|(DWORD)lround(blue);
    }
    return bitmap;
}
static void login_frame_sync(HWND dlg) {
    LoginFrame *frame=(LoginFrame*)GetPropW(dlg,LOGIN_FRAME_PROP);
    if(!frame)return;
    RECT r;GetWindowRect(dlg,&r);
    int w=r.right-r.left,h=r.bottom-r.top,dpi=GetDpiForWindow(dlg),pad=MulDiv(26,dpi,96);
    BOOL active=GetForegroundWindow()==dlg;
    if(w<=0 || h<=0)return;
    if(!frame->painted || frame->width!=w || frame->height!=h || frame->dpi!=dpi || frame->active!=active){
        DWORD *data=NULL;HBITMAP bitmap=login_frame_bitmap(dlg,w,h,pad,active,&data);
        if(bitmap){
            HDC dc=CreateCompatibleDC(NULL);HGDIOBJ old=SelectObject(dc,bitmap);
            POINT dest={r.left-pad,r.top-pad},source={0};SIZE size={w+pad*2,h+pad*2};
            BLENDFUNCTION blend={AC_SRC_OVER,0,255,AC_SRC_ALPHA};
            frame->painted=UpdateLayeredWindow(frame->hwnd,NULL,&dest,&size,dc,&source,0,&blend,ULW_ALPHA);
            SelectObject(dc,old);DeleteDC(dc);DeleteObject(bitmap);
            frame->width=w;frame->height=h;frame->dpi=dpi;frame->active=active;
        }
    }
    BOOL visible=(GetWindowLongPtrW(dlg,GWL_STYLE)&WS_VISIBLE) && !IsIconic(dlg);
    SetWindowPos(frame->hwnd,dlg,r.left-pad,r.top-pad,w+pad*2,h+pad*2,
                 SWP_NOACTIVATE|(visible && frame->painted?SWP_SHOWWINDOW:SWP_HIDEWINDOW));
}
static void login_frame_attach(HWND dlg) {
    WNDCLASSW wc={0};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(NULL);wc.lpszClassName=L"CalabiLoginFrame";
    if(!RegisterClassW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return;
    LoginFrame *frame=calloc(1,sizeof(*frame));if(!frame)return;
    // Unowned: an owned popup would be forced above the dialog. Position this
    // tool window directly behind it and explicitly mirror the dialog lifetime.
    frame->hwnd=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,
        wc.lpszClassName,L"",WS_POPUP,0,0,0,0,NULL,NULL,wc.hInstance,NULL);
    if(!frame->hwnd || !SetPropW(dlg,LOGIN_FRAME_PROP,frame)){
        if(frame->hwnd)DestroyWindow(frame->hwnd);free(frame);return;
    }
    login_frame_sync(dlg);
}
static void login_frame_destroy(HWND dlg) {
    LoginFrame *frame=(LoginFrame*)RemovePropW(dlg,LOGIN_FRAME_PROP);
    if(frame){DestroyWindow(frame->hwnd);free(frame);}
}
