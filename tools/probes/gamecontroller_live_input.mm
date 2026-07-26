#import <Foundation/Foundation.h>
#include "input/api/GameController/GCController_mac.h"
#include <cstdio>
#include <string>
int main() {
    @autoreleasepool {
        GCBridge::Initialize();
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
        auto devs = GCBridge::EnumerateDevices();
        if (devs.empty()) { printf("no controller\n"); return 1; }
        printf(">>> Press buttons / move sticks on the %s for 20 seconds <<<\n\n", devs[0].displayName.c_str());
        fflush(stdout);
        std::string last;
        for (int i = 0; i < 200; i++) {
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
            GCBridge::GamepadState s;
            if (!GCBridge::ReadState(devs[0].uuid, s)) continue;
            std::string cur;
            if (s.a) cur += "A ";
            if (s.b) cur += "B ";
            if (s.x) cur += "X ";
            if (s.y) cur += "Y ";
            if (s.up) cur += "Up ";
            if (s.down) cur += "Down ";
            if (s.left) cur += "Left ";
            if (s.right) cur += "Right ";
            if (s.leftShoulder) cur += "LB ";
            if (s.rightShoulder) cur += "RB ";
            if (s.menu) cur += "Menu ";
            if (s.options) cur += "Options ";
            if (s.leftThumbstick) cur += "LS ";
            if (s.rightThumbstick) cur += "RS ";
            char buf[160];
            snprintf(buf, sizeof(buf), "%sLX=%.2f LY=%.2f RX=%.2f RY=%.2f LT=%.2f RT=%.2f",
                     cur.c_str(), s.leftStickX, s.leftStickY, s.rightStickX, s.rightStickY,
                     s.leftTrigger, s.rightTrigger);
            if (buf != last) { printf("%s\n", buf); fflush(stdout); last = buf; }
        }
        GCBridge::Shutdown();
    }
    return 0;
}
