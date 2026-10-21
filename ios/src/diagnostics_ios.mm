#import <UIKit/UIKit.h>
#import <AVFAudio/AVFAudio.h>
#include "host/lifecycle.h"
#include "audio/output.h"
#include "host/touch.h"
#include "log.h"

namespace {
void uncaught(NSException* exception) {
    LOG("FATAL NSException: %s: %s\n%s", exception.name.UTF8String,
        exception.reason.UTF8String, exception.callStackSymbols.description.UTF8String);
}
void observe(NSString* name, bool active) {
    [NSNotificationCenter.defaultCenter addObserverForName:name object:nil queue:nil usingBlock:^(NSNotification* note) {
        LOG("ios: notification %s app_state=%ld", note.name.UTF8String, (long)UIApplication.sharedApplication.applicationState);
        if (!active) { audio::set_app_active(false); touch::cancel_all(); }
        lifecycle::set_active(active, note.name.UTF8String);
        if (active) audio::set_app_active(true);
    }];
}
}

void botw_ios_install_diagnostics() {
    NSSetUncaughtExceptionHandler(uncaught);
    // UIScene notifications also cover pulling down Control / Notification Center.
    observe(UIApplicationWillResignActiveNotification, false);
    observe(UIApplicationDidEnterBackgroundNotification, false);
    observe(UIApplicationDidBecomeActiveNotification, true);
    observe(UISceneWillDeactivateNotification, false);
    observe(UISceneDidEnterBackgroundNotification, false);
    observe(UISceneDidActivateNotification, true);
    auto center = NSNotificationCenter.defaultCenter;
    [center addObserverForName:UIApplicationDidReceiveMemoryWarningNotification object:nil queue:nil usingBlock:^(NSNotification*) {
        LOG("ios: MEMORY WARNING active=%d", lifecycle::active());
    }];
    [center addObserverForName:NSProcessInfoThermalStateDidChangeNotification object:nil queue:nil usingBlock:^(NSNotification*) {
        LOG("ios: thermal state changed to %ld (0=nominal 1=fair 2=serious 3=critical)", (long)NSProcessInfo.processInfo.thermalState);
    }];
    [center addObserverForName:AVAudioSessionInterruptionNotification object:nil queue:nil usingBlock:^(NSNotification* note) {
        bool began = [note.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue] == AVAudioSessionInterruptionTypeBegan;
        // Always defer: SDL can post this while holding its audio / stream locks.
        dispatch_async(dispatch_get_main_queue(), ^{
            LOG("ios: audio interruption %s", began ? "began" : "ended");
            audio::set_interrupted(began);
        });
    }];
    [center addObserverForName:AVAudioSessionRouteChangeNotification object:nil queue:nil usingBlock:^(NSNotification* note) {
        NSUInteger reason = [note.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue];
        dispatch_async(dispatch_get_main_queue(), ^{
            LOG("ios: audio route change reason=%lu route=%s", (unsigned long)reason,
                AVAudioSession.sharedInstance.currentRoute.description.UTF8String);
            audio::route_changed();
        });
    }];
    LOG("ios: diagnostics installed; scene/app focus, memory, thermal and audio notifications enabled");
}
