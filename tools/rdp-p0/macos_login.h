// AppKit presentation of the shared Windows login design.
#include "rdp_locale.h"
#include "probe.h"
#include <math.h>
static NSColor *LoginColor(int r, int g, int b) {
    return [NSColor colorWithSRGBRed:r/255.0 green:g/255.0 blue:b/255.0 alpha:1];
}

static unsigned LoginPortValue(NSString *text) {
    NSCharacterSet *nonDigits = [NSCharacterSet characterSetWithCharactersInString:@"0123456789"].invertedSet;
    if (!text.length || text.length > 5 || [text rangeOfCharacterFromSet:nonDigits].location != NSNotFound) return 0;
    unsigned port = (unsigned)text.intValue;
    return port <= UINT16_MAX ? port : 0;
}

@interface LoginPanel : NSPanel
@end
@implementation LoginPanel
- (BOOL)canBecomeKeyWindow { return YES; }
@end

@interface LoginSurface : NSView
@end
@implementation LoginSurface
- (BOOL)isFlipped { return YES; }
- (BOOL)mouseDownCanMoveWindow { return YES; }
- (void)drawRect:(NSRect)dirty {
    [NSGraphicsContext saveGraphicsState];
    [[NSBezierPath bezierPathWithRoundedRect:self.bounds xRadius:18 yRadius:18] addClip];
    NSGradient *body = [[NSGradient alloc] initWithStartingColor:LoginColor(248,251,255) endingColor:LoginColor(240,245,251)];
    [body drawInRect:self.bounds angle:45];
    NSGradient *header = [[NSGradient alloc] initWithStartingColor:LoginColor(34,60,91) endingColor:LoginColor(23,40,62)];
    [header drawInRect:NSMakeRect(0,0,self.bounds.size.width,104) angle:0];
    NSBezierPath *card = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(18,120,364,133) xRadius:11 yRadius:11];
    [NSColor.whiteColor setFill]; [card fill];
    [LoginColor(226,232,240) setStroke]; [card stroke];
    for (NSView *view in self.subviews) {
        if (![view.identifier isEqualToString:@"login-input"]) continue;
        NSBezierPath *box = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(view.frame,-6,-5) xRadius:6 yRadius:6];
        [NSColor.whiteColor setFill]; [box fill];
        BOOL focused = [view isKindOfClass:NSTextField.class] && [(NSTextField *)view currentEditor] != nil;
        [(focused ? LoginColor(115,149,207) : LoginColor(220,228,239)) setStroke]; [box stroke];
    }
    NSBezierPath *icon = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(20,48,36,36) xRadius:10 yRadius:10];
    [LoginColor(38,63,91) setFill]; [icon fill];
    [LoginColor(69,92,119) setStroke]; [icon stroke];
    [LoginColor(211,231,255) setStroke];
    [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(28,57,20,14) xRadius:2 yRadius:2] stroke];
    [LoginColor(211,231,255) setFill]; NSRectFill(NSMakeRect(33,75,10,1));
    [NSGraphicsContext restoreGraphicsState];
}
@end

@interface LoginButton : NSButton
@property BOOL primary;
@property BOOL secondary;
@property BOOL header;
@end
@implementation LoginButton
- (void)drawRect:(NSRect)dirty {
    if (self.primary) {
        [(!self.enabled ? LoginColor(140,160,194) : self.highlighted ? LoginColor(51,86,150) : LoginColor(66,107,180)) setFill];
        [[NSBezierPath bezierPathWithRoundedRect:self.bounds xRadius:7 yRadius:7] fill];
    } else if (self.secondary) {
        NSBezierPath *box = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds,.5,.5) xRadius:7 yRadius:7];
        [(self.highlighted ? LoginColor(237,242,248) : NSColor.whiteColor) setFill]; [box fill];
        [LoginColor(217,225,237) setStroke]; [box stroke];
    }
    NSColor *color = self.primary ? NSColor.whiteColor : self.header ? LoginColor(198,210,227) : LoginColor(83,100,123);
    NSDictionary *attributes = @{NSFontAttributeName:self.font,
                                  NSForegroundColorAttributeName:color};
    NSSize size = [self.title sizeWithAttributes:attributes];
    [self.title drawAtPoint:NSMakePoint((self.bounds.size.width-size.width)/2,
                                      (self.bounds.size.height-size.height)/2) withAttributes:attributes];
    if (self.window.firstResponder == self) {
        [LoginColor(115,149,207) setStroke];
        [[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds,3,3) xRadius:5 yRadius:5] stroke];
    }
}
@end

@interface LoginForm : NSObject <NSWindowDelegate, NSTextFieldDelegate>
@property(nonatomic, strong) LoginPanel *panel;
@property(nonatomic, strong) NSTextField *account;
@property(nonatomic, strong) NSSecureTextField *password;
@property(nonatomic, strong) NSTextField *port;
@property(nonatomic, strong) NSTextField *hint;
@property(nonatomic, strong) LoginButton *connectButton;
@property(nonatomic, strong) LoginButton *cancelButton;
@property(nonatomic, strong) NSButton *files;
@property(nonatomic, strong) NSButton *remember;
@property(nonatomic, strong) NSButton *forgetButton;
@property(nonatomic, copy) void (^submit)(unsigned port);
@property(nonatomic, copy) void (^dismiss)(void);
@property(nonatomic, copy) void (^forget)(unsigned port);
@property(nonatomic, readonly) BOOL ssh;
@property BOOL connecting;
- (instancetype)initWithName:(NSString *)name host:(NSString *)host ssh:(BOOL)ssh;
- (void)applyPortHint:(NSString *)hint;
- (void)setStatus:(NSString *)status;
- (void)setPasswordHint:(NSString *)hint;
@end
@implementation LoginForm
- (NSTextField *)label:(NSString *)text rect:(NSRect)rect size:(CGFloat)size color:(NSColor *)color {
    NSTextField *label = [NSTextField labelWithString:text];
    label.frame = rect; label.font = [NSFont systemFontOfSize:size];
    label.textColor = color; label.lineBreakMode = NSLineBreakByTruncatingTail;
    [self.panel.contentView addSubview:label];
    return label;
}
- (LoginButton *)button:(NSString *)text rect:(NSRect)rect action:(SEL)action {
    LoginButton *button = [[LoginButton alloc] initWithFrame:rect];
    button.title = text; button.bordered = NO;
    button.font = [NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
    button.target = self; button.action = action;
    [self.panel.contentView addSubview:button];
    return button;
}
- (instancetype)initWithName:(NSString *)name host:(NSString *)host {
    return [self initWithName:name host:host ssh:NO];
}
- (instancetype)initWithName:(NSString *)name host:(NSString *)host ssh:(BOOL)ssh {
    if ((self = [super init])) {
        _ssh = ssh;
        CGFloat height = ssh ? 334 : 358;
        self.panel = [[LoginPanel alloc] initWithContentRect:NSMakeRect(0,0,400,height)
            styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
        self.panel.delegate = self;
        self.panel.releasedWhenClosed = NO;
        self.panel.opaque = NO; self.panel.backgroundColor = NSColor.clearColor;
        self.panel.hasShadow = YES; self.panel.movableByWindowBackground = YES;
        self.panel.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
        LoginSurface *surface = [[LoginSurface alloc] initWithFrame:NSMakeRect(0,0,400,height)];
        surface.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        self.panel.contentView = surface;
        [self label:RDP_NS(Login) rect:NSMakeRect(20,13,330,18) size:11 color:LoginColor(193,206,221)];
        NSTextField *device = [self label:name.length ? name : RDP_NS(Device)
            rect:NSMakeRect(67,45,310,27) size:20 color:LoginColor(244,247,252)];
        device.font = [NSFont systemFontOfSize:20 weight:NSFontWeightMedium];
        [self label:host rect:NSMakeRect(67,75,310,17) size:11 color:LoginColor(181,198,217)];
        NSArray<NSString *> *fieldLabels = @[RDP_NS(Account), RDP_NS(Password), RDP_NS(Port)];
        const CGFloat labelX = 30, fontSize = 12;
        NSFont *fieldFont = [NSFont systemFontOfSize:fontSize];
        NSDictionary *labelAttributes = @{NSFontAttributeName:fieldFont};
        CGFloat labelWidth = 0;
        for (NSString *label in fieldLabels) {
            CGFloat width = ceil([label sizeWithAttributes:labelAttributes].width);
            labelWidth = MAX(labelWidth, width);
        }
        labelWidth += 4; // AppKit label insets.
        CGFloat fieldX = labelX + labelWidth + 11, fieldWidth = 364 - fieldX;
        for (NSUInteger row = 0; row < fieldLabels.count; ++row)
            [self label:fieldLabels[row] rect:NSMakeRect(labelX,136+40*row,labelWidth,20) size:fontSize color:LoginColor(95,109,128)];
        self.account = [[NSTextField alloc] initWithFrame:NSMakeRect(fieldX,135,fieldWidth,23)];
        self.password = [[NSSecureTextField alloc] initWithFrame:NSMakeRect(fieldX,175,fieldWidth,23)];
        self.port = [[NSTextField alloc] initWithFrame:NSMakeRect(fieldX,215,fieldWidth,23)];
        for (NSTextField *field in @[self.account,self.password,self.port]) {
            field.bordered = NO; field.drawsBackground = NO;
            field.textColor = LoginColor(38,55,75);
            field.font = [NSFont systemFontOfSize:13];
            field.identifier = @"login-input";
            field.focusRingType = NSFocusRingTypeNone;
            field.delegate = self;
            [self.panel.contentView addSubview:field];
        }
        self.account.placeholderString = ssh ? @"" : RDP_NS(AccountPlaceholder);
        [self setPasswordHint:ssh ? RDP_NS(SshSavedOrKeyHint) : RDP_NS(PasswordPlaceholder)];
        self.account.accessibilityLabel = fieldLabels[0];
        self.password.accessibilityLabel = fieldLabels[1];
        self.port.accessibilityLabel = fieldLabels[2];
        [self applyPortHint:nil];
        self.hint = [self label:@"" rect:NSMakeRect(20,260,360,34) size:11 color:LoginColor(159,69,44)];
        self.hint.cell.usesSingleLineMode = NO;
        self.hint.maximumNumberOfLines = 2;
        self.hint.lineBreakMode = NSLineBreakByWordWrapping;
        if (!ssh) {
            self.files = [NSButton checkboxWithTitle:RDP_NS(AllowFiles) target:nil action:nil];
            self.files.state = NSControlStateValueOn;
            self.files.frame = NSMakeRect(20,0,360,22);
            self.files.font = [NSFont systemFontOfSize:12];
            [self.panel.contentView addSubview:self.files];
        }
        self.remember = [NSButton checkboxWithTitle:RDP_NS(Remember) target:nil action:nil];
        self.remember.font = [NSFont systemFontOfSize:12];
        self.remember.toolTip = self.remember.title;
        NSString *clear = RDP_NS(ClearCredentials);
        NSFont *clearFont = [NSFont systemFontOfSize:11 weight:NSFontWeightMedium];
        CGFloat clearWidth = ceil([clear sizeWithAttributes:@{NSFontAttributeName:clearFont}].width) + 16;
        self.remember.frame = NSMakeRect(20,0,360-clearWidth-6,22);
        [self.panel.contentView addSubview:self.remember];
        self.forgetButton = [self button:clear rect:NSMakeRect(380-clearWidth,0,clearWidth,26) action:@selector(forgetPassword:)];
        self.forgetButton.font = clearFont;
        NSDictionary *buttonAttributes = @{NSFontAttributeName:[NSFont systemFontOfSize:13 weight:NSFontWeightMedium]};
        CGFloat connectWidth = MAX(108,ceil(MAX([RDP_NS(Connect) sizeWithAttributes:buttonAttributes].width,
            [RDP_NS(Connecting) sizeWithAttributes:buttonAttributes].width))+24);
        CGFloat cancelWidth = MAX(68,ceil([RDP_NS(Cancel) sizeWithAttributes:buttonAttributes].width)+24);
        LoginButton *cancel = [self button:RDP_NS(Cancel) rect:NSMakeRect(372-connectWidth-cancelWidth,0,cancelWidth,33) action:@selector(cancel:)];
        cancel.secondary = YES;
        cancel.keyEquivalent = @"\033";
        self.cancelButton = cancel;
        LoginButton *connect = [self button:RDP_NS(Connect) rect:NSMakeRect(380-connectWidth,0,connectWidth,33) action:@selector(connect:)];
        connect.primary = YES; connect.keyEquivalent = @"\r";
        self.connectButton = connect;
        LoginButton *close = [self button:@"×" rect:NSMakeRect(357,10,26,26) action:@selector(cancel:)];
        close.header = YES;
        close.accessibilityLabel = RDP_NS(Close);
        self.account.nextKeyView = self.password;
        self.password.nextKeyView = self.port;
        self.port.nextKeyView = self.files ?: self.remember;
        self.files.nextKeyView = self.remember;
        self.remember.nextKeyView = self.forgetButton; self.forgetButton.nextKeyView = connect;
        connect.nextKeyView = cancel; cancel.nextKeyView = self.account;
        self.panel.initialFirstResponder = self.account;
        [self setStatus:@""];
    }
    return self;
}
- (void)applyPortHint:(NSString *)hint {
    unsigned port = LoginPortValue(hint);
    self.port.stringValue = [NSString stringWithFormat:@"%u", port ?: (self.ssh ? 22u : PROBE_RDP_DEFAULT_PORT)];
}
- (void)setPasswordHint:(NSString *)hint {
    self.password.placeholderString = hint;
    self.password.toolTip = hint;
}
- (void)setStatus:(NSString *)status {
    self.hint.stringValue = status;
    self.hint.hidden = !status.length;
    CGFloat rememberTop = NSMinY(self.hint.frame) + 6 + (status.length ? NSHeight(self.hint.frame) : 0);
    if (self.files) {
        [self.files setFrameOrigin:NSMakePoint(20,rememberTop)];
        rememberTop += 24;
    }
    [self.remember setFrameOrigin:NSMakePoint(NSMinX(self.remember.frame),rememberTop)];
    [self.forgetButton setFrameOrigin:NSMakePoint(NSMinX(self.forgetButton.frame),rememberTop-2)];
    CGFloat buttonTop = rememberTop + 26;
    [self.cancelButton setFrameOrigin:NSMakePoint(NSMinX(self.cancelButton.frame),buttonTop)];
    [self.connectButton setFrameOrigin:NSMakePoint(NSMinX(self.connectButton.frame),buttonTop)];
    NSRect frame = self.panel.frame;
    CGFloat height = buttonTop + 42;
    frame.origin.y = NSMaxY(frame) - height;
    frame.size.height = height;
    [self.panel setFrame:frame display:self.panel.visible];
}
- (unsigned)validatedPort {
    unsigned port = LoginPortValue(self.port.stringValue);
    if (!port) {
        [self setStatus:RDP_NS(InvalidPort)];
        [self.panel makeFirstResponder:self.port];
    }
    return port;
}
- (void)connect:(id)sender {
    if (self.connecting) return;
    if (![self.panel makeFirstResponder:nil]) return;
    if (!self.account.stringValue.length || [self.account.stringValue hasSuffix:@"\\"]) {
        [self setStatus:self.ssh ? RDP_NS(SshInvalidLogin) : RDP_NS(EnterAccount)];
        [self.panel makeFirstResponder:self.account];
        return;
    }
    unsigned port = [self validatedPort];
    if (!port) return;
    self.connecting = YES;
    self.account.enabled = NO; self.password.enabled = NO; self.port.enabled = NO;
    self.files.enabled = NO;
    self.remember.enabled = NO; self.forgetButton.enabled = NO;
    self.connectButton.enabled = NO;
    self.connectButton.title = RDP_NS(Connecting);
    [self setStatus:RDP_NS(CheckingLogin)];
    if (self.submit) self.submit(port);
}
- (void)failed:(NSString *)message {
    self.connecting = NO;
    self.account.enabled = YES; self.password.enabled = YES; self.port.enabled = YES;
    self.files.enabled = YES;
    self.remember.enabled = YES; self.forgetButton.enabled = YES;
    self.connectButton.enabled = YES; self.connectButton.title = RDP_NS(Connect);
    [self setStatus:message];
    [self.panel makeFirstResponder:self.password];
}
- (void)forgetPassword:(id)sender {
    if (self.connecting || !self.forget || ![self.panel makeFirstResponder:nil]) return;
    unsigned port = [self validatedPort];
    if (port) self.forget(port);
}
- (void)controlTextDidBeginEditing:(NSNotification *)notification {
    NSTextField *field = notification.object;
    if (field == self.account) field.placeholderString = @"";
    NSTextView *editor = (NSTextView *)field.currentEditor;
    editor.insertionPointColor = LoginColor(38,55,75);
    self.panel.contentView.needsDisplay = YES;
}
- (void)controlTextDidEndEditing:(NSNotification *)notification {
    NSTextField *field = notification.object;
    if (field == self.account) field.placeholderString = self.ssh ? @"" : RDP_NS(AccountPlaceholder);
    self.panel.contentView.needsDisplay = YES;
}
- (void)cancel:(id)sender { if (self.dismiss) self.dismiss(); }
- (BOOL)windowShouldClose:(NSWindow *)sender {
    [self cancel:sender];
    return NO;
}
- (void)show {
    self.panel.hidesOnDeactivate = NO;
    self.panel.collectionBehavior = NSWindowCollectionBehaviorMoveToActiveSpace |
                                    NSWindowCollectionBehaviorFullScreenAuxiliary;
    [self.panel center];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(activated:)
        name:NSApplicationDidBecomeActiveNotification object:NSApp];
    [self.panel orderFrontRegardless];
    [NSApp activateIgnoringOtherApps:YES];
    [self focusAccount];
}
- (void)activated:(NSNotification *)notification {
    [[NSNotificationCenter defaultCenter] removeObserver:self
        name:NSApplicationDidBecomeActiveNotification object:NSApp];
    if (self.panel.visible) [self focusAccount];
}
- (void)focusAccount {
    [self.panel makeKeyAndOrderFront:nil];
    [self.panel makeFirstResponder:self.account];
    self.account.placeholderString = @"";
    NSTextView *editor = (NSTextView *)self.account.currentEditor;
    editor.insertionPointColor = LoginColor(38,55,75);
}
- (void)close {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [self.panel orderOut:nil];
    self.panel.delegate = nil;
    [self.panel close];
}
@end
