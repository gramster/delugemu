// SPDX-License-Identifier: GPL-2.0-or-later
//
// Minimal startup splash for the DelugEmu.app launcher: a floating panel with
// a spinner shown while the launcher does its pre-boot work (downloads, SD
// card image build) so a first launch doesn't look hung. The launcher kills
// this process (SIGTERM) right before the emulator window appears.
//
// Built by scripts/package.sh:  cc -o delugemu_splash splash.m -framework Cocoa

#import <Cocoa/Cocoa.h>

int main(void)
{
    @autoreleasepool {
        [NSApplication sharedApplication];

        NSWindow *w = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, 430, 110)
                      styleMask:NSWindowStyleMaskTitled
                        backing:NSBackingStoreBuffered
                          defer:NO];
        [w setTitle:@"DelugEmu"];
        [w setLevel:NSFloatingWindowLevel];
        [w center];

        NSProgressIndicator *spin = [[NSProgressIndicator alloc]
            initWithFrame:NSMakeRect(24, 34, 42, 42)];
        [spin setStyle:NSProgressIndicatorStyleSpinning];
        [spin startAnimation:nil];
        [[w contentView] addSubview:spin];

        NSTextField *label = [NSTextField labelWithString:
            @"Starting DelugEmu…\n"
            @"The window opens when the emulator is ready. A first launch\n"
            @"can take a few minutes (downloads and SD card build)."];
        [label setFrame:NSMakeRect(84, 18, 330, 74)];
        [[w contentView] addSubview:label];

        [w makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
    }
    return 0;
}
