// Offscreen AppKit forms only: no credential store, network or remote session.
#import <AppKit/AppKit.h>
#include <assert.h>
#include <stdio.h>
#include "macos_login.h"

int main(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
        unsetenv("CALABI_UI_LOCALE_FILE");
        for (int ssh = 0; ssh < 2; ++ssh) for (int locale = 0; locale < RdpLocaleCount; ++locale) {
            @autoreleasepool {
                setenv("CALABI_UI_LOCALE", rdp_locale_codes[locale], 1);
                LoginForm *form = [[LoginForm alloc] initWithName:@"fixture-device" host:@"127.0.0.1" ssh:ssh];
                assert(form.panel && !form.panel.visible && !form.panel.opaque);
                assert([form.panel.appearance.name isEqualToString:NSAppearanceNameAqua]);
                assert([form.account.accessibilityLabel isEqualToString:RDP_NS(Account)]);
                assert([form.password.accessibilityLabel isEqualToString:RDP_NS(Password)]);
                assert([form.port.accessibilityLabel isEqualToString:RDP_NS(Port)]);
                assert(NSMinX(form.account.frame) == NSMinX(form.password.frame));
                assert(NSMinX(form.account.frame) == NSMinX(form.port.frame));
                assert(NSMaxX(form.account.frame) == NSMaxX(form.port.frame));
                assert(NSMaxY(form.account.frame) < NSMinY(form.password.frame));
                assert(NSMaxY(form.password.frame) < NSMinY(form.port.frame));
                assert(!form.hint.stringValue.length && form.hint.hidden);
                assert((form.files == nil) == (ssh != 0));
                if (form.files) assert(form.files.state == NSControlStateValueOn);
                for (NSView *view in form.panel.contentView.subviews) {
                    if (![view isKindOfClass:NSTextField.class] || view == form.hint) continue;
                    NSTextField *label = (NSTextField *)view;
                    if (label.editable || NSMinY(label.frame) < 120) continue;
                    assert(NSMaxX(label.frame) < NSMinX(form.account.frame));
                }
                NSDictionary *font = @{NSFontAttributeName:form.connectButton.font};
                assert([RDP_NS(Connecting) sizeWithAttributes:font].width < NSWidth(form.connectButton.frame));
                assert(NSMaxX(form.cancelButton.frame) < NSMinX(form.connectButton.frame));
                assert(NSMaxY(form.connectButton.frame) < NSHeight(form.panel.contentView.bounds));
                CGFloat top = NSMaxY(form.panel.frame), height = NSHeight(form.panel.frame);
                [form setStatus:RDP_NS(VerifyFailed)];
                assert(!form.hint.hidden && NSHeight(form.panel.frame) > height);
                assert(fabs(NSMaxY(form.panel.frame)-top) < 0.01);
                assert(NSMinY(form.remember.frame) > NSMaxY(form.hint.frame));
                [form setStatus:@""];
                assert(NSHeight(form.panel.frame) == height);
                [form applyPortHint:@"65536"];
                assert([form.port.stringValue intValue] == (ssh ? 22 : PROBE_RDP_DEFAULT_PORT));
                form.port.stringValue = @"0";
                assert([form validatedPort] == 0 && !form.hint.hidden);
                form.account.stringValue = @"fixture-user";
                form.password.stringValue = @"fixture-password";
                form.port.stringValue = @"3391";
                __block unsigned submitted = 0;
                form.submit = ^(unsigned port) { submitted = port; };
                [form connect:nil];
                assert(submitted == 3391 && form.connecting);
                assert(!form.account.enabled && !form.password.enabled && !form.port.enabled);
                [form failed:RDP_NS(LoginFailed)];
                assert(!form.connecting && form.account.enabled && form.password.enabled && form.port.enabled);
                assert([form.account.stringValue isEqualToString:@"fixture-user"]);
                __block BOOL dismissed = NO;
                form.dismiss = ^{ dismissed = YES; };
                [form cancel:nil]; assert(dismissed);
                [form close];
            }
        }
        puts("Mac login UI passed: 10 locales, compact columns, status resize, port validation, retry and cancel.");
    }
    return 0;
}
