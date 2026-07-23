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

        /* Size the window around the label's own fitted size so the text can
         * never truncate, whatever the font metrics. */
        NSTextField *label = [NSTextField labelWithString:
            @"Starting DelugEmu…\n"
            @"The window opens when the emulator is ready. A first launch\n"
            @"can take a few minutes (downloads and SD card build)."];
        NSSize ls = [label frame].size;
        CGFloat width = 84 + ls.width + 24;
        CGFloat height = MAX(94, ls.height + 36);

        NSWindow *w = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, width, height)
                      styleMask:NSWindowStyleMaskTitled
                        backing:NSBackingStoreBuffered
                          defer:NO];
        [w setTitle:@"DelugEmu"];
        [w setLevel:NSFloatingWindowLevel];
        [w center];

        NSProgressIndicator *spin = [[NSProgressIndicator alloc]
            initWithFrame:NSMakeRect(24, (height - 42) / 2, 42, 42)];
        [spin setStyle:NSProgressIndicatorStyleSpinning];
        [spin startAnimation:nil];
        [[w contentView] addSubview:spin];

        [label setFrame:NSMakeRect(84, (height - ls.height) / 2,
                                   ls.width, ls.height)];
        [[w contentView] addSubview:label];

        [w makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
    }
    return 0;
}
