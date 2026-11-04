// Native Touch: send the fingers on the iPad's screen to the
// VMM's Apple Virtual Platform touch screen, so macOS sees a native touch screen.
//
// A HID monitor client reads the fingers instead of UIKit because iPadOS holds
// back and cancels UIKit touches near its screen edges and during its multitasking
// gestures, which are the same motions macOS uses for its own system gestures.
// UIKit still decides who a contact belongs to: one that lands on anything but the
// guest's view is left to the app. HID also sees fingers on other apps' windows,
// which this can't tell apart, so Native Touch doesn't support Stage Manager.
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#import <objc/message.h>
#include <math.h>

#import "VZTouchScreenRelay.h"

typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;
typedef struct __IOHIDEvent *IOHIDEventRef;
extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreateWithType(CFAllocatorRef, int, CFDictionaryRef);
extern void IOHIDEventSystemClientRegisterEventCallback(
    IOHIDEventSystemClientRef, void (*)(void *, void *, IOHIDServiceClientRef, IOHIDEventRef), void *, void *);
extern void IOHIDEventSystemClientUnregisterEventCallback(
    IOHIDEventSystemClientRef, void (*)(void *, void *, IOHIDServiceClientRef, IOHIDEventRef), void *, void *);
extern void IOHIDEventSystemClientScheduleWithRunLoop(IOHIDEventSystemClientRef, CFRunLoopRef, CFStringRef);
extern void IOHIDEventSystemClientUnscheduleWithRunLoop(IOHIDEventSystemClientRef, CFRunLoopRef, CFStringRef);
extern uint32_t IOHIDEventGetType(IOHIDEventRef);
extern CFArrayRef IOHIDEventGetChildren(IOHIDEventRef);
extern CFIndex IOHIDEventGetIntegerValue(IOHIDEventRef, uint32_t);
extern double IOHIDEventGetFloatValue(IOHIDEventRef, uint32_t);

enum { kMonitorClient = 1, kDigitizerEvent = 11 };
#define FIELD(index) (((uint32_t)kDigitizerEvent << 16) | (index))
enum { kFieldX = 0, kFieldY = 1, kFieldType = 4, kFieldPath = 5, kFieldRange = 8, kFieldTouch = 9 };
enum { kTypeStylus = 0 };
enum { kPhaseBegan = 0, kPhaseMoved = 1, kPhaseEnded = 3 };

// The VMM keeps its contacts in a bitset of 6 and aborts on a larger index.
// Slots 1-6 are sent as touch indexes 0-5; further contacts are ignored.
enum { kSlots = 6, kPaths = 32 };

// A point exactly on the far edge of the display starts no system gesture.
static const double kEdgeInset = 2;
// A fast swipe leaves a system gesture's edge region before the gesture is
// recognized, so a contact that lands near an edge stays close to it briefly and
// then catches up with the finger.
static const double kEdgeZone = 50, kEdgeHoldSeconds = 0.06, kEdgeStart = 6, kEdgePointsPerSecond = 250;

typedef enum { kEdgeNone, kEdgeLeft, kEdgeRight, kEdgeTop, kEdgeBottom } TouchEdge;

typedef struct {
    uint8_t path;
    TouchEdge edge;
    CFTimeInterval began;
    CGPoint location;  // normalized in the guest display, before shaping
    CGPoint sent;
} Contact;

static IOHIDEventSystemClientRef gClient;
static id gTouchDevice;
static VZTouchScreenViewportProvider gViewportProvider;
static Contact gContacts[kSlots + 1];
static uint32_t gSentSlots;
static uint32_t gIgnoredPaths;
// The on-screen keyboard, in screen coordinates. Its window isn't the guest's, and
// HID reports the fingers typing on it like any others.
static CGRect gKeyboardFrame;
static id gKeyboardObserver;

static TouchEdge nearestEdge(CGPoint location, CGSize size) {
    double left = location.x * size.width, right = (1 - location.x) * size.width;
    double top = location.y * size.height, bottom = (1 - location.y) * size.height;
    double nearest = fmin(fmin(left, right), fmin(top, bottom));
    if (nearest > kEdgeZone) return kEdgeNone;
    if (nearest == right) return kEdgeRight;
    if (nearest == left) return kEdgeLeft;
    if (nearest == top) return kEdgeTop;
    return kEdgeBottom;
}

static CGPoint shapedLocation(const Contact *contact, CFTimeInterval now, CGSize size) {
    double x = contact->location.x, y = contact->location.y;
    CFTimeInterval age = now - contact->began;
    if (contact->edge != kEdgeNone && age < kEdgeHoldSeconds) {
        double limit = kEdgeStart + kEdgePointsPerSecond * age;
        switch (contact->edge) {
            case kEdgeLeft: x = fmin(x, limit / size.width); break;
            case kEdgeRight: x = fmax(x, 1 - limit / size.width); break;
            case kEdgeTop: y = fmin(y, limit / size.height); break;
            case kEdgeBottom: y = fmax(y, 1 - limit / size.height); break;
            case kEdgeNone: break;
        }
    }
    x = fmin(fmax(x, kEdgeInset / size.width), 1 - kEdgeInset / size.width);
    y = fmin(fmax(y, kEdgeInset / size.height), 1 - kEdgeInset / size.height);
    return CGPointMake(x, y);
}

// The digitizer reports positions in the screen's native portrait orientation.
static CGPoint interfacePoint(UIInterfaceOrientation orientation, double x, double y) {
    switch (orientation) {
        case UIInterfaceOrientationLandscapeRight: return CGPointMake(y, 1 - x);
        case UIInterfaceOrientationLandscapeLeft: return CGPointMake(1 - y, x);
        case UIInterfaceOrientationPortraitUpsideDown: return CGPointMake(1 - x, 1 - y);
        default: return CGPointMake(x, y);
    }
}

// Whether a window above the guest's, such as a floating keyboard's, would take a
// touch at this point.
static BOOL windowAboveTakesPoint(UIWindowScene *scene, UIWindow *guestWindow, CGPoint inScreen) {
    for (UIWindow *window in scene.windows) {
        if (window == guestWindow || window.hidden || window.windowLevel <= guestWindow.windowLevel) continue;
        CGPoint inWindow = [scene.screen.coordinateSpace convertPoint:inScreen toCoordinateSpace:window];
        if ([window hitTest:inWindow withEvent:nil]) return YES;
    }
    return NO;
}

// Where a finger is on the guest's display, normalized. landsOnGuest says whether
// a touch there would go to the guest's view rather than something else in this app.
static BOOL locateFinger(double x, double y, CGPoint *location, CGSize *size, BOOL *landsOnGuest) {
    UIView *view = nil;
    CGRect viewport = gViewportProvider ? gViewportProvider(&view) : CGRectZero;
    UIWindowScene *scene = view.window.windowScene;
    if (!scene || viewport.size.width <= 0 || viewport.size.height <= 0) return NO;
    CGPoint normalized = interfacePoint(scene.interfaceOrientation, x, y);
    CGRect screen = scene.screen.bounds;
    CGPoint inScreen = CGPointMake(normalized.x * screen.size.width, normalized.y * screen.size.height);
    CGPoint inView = [scene.screen.coordinateSpace convertPoint:inScreen toCoordinateSpace:view];
    *location = CGPointMake(fmin(fmax((inView.x - viewport.origin.x) / viewport.size.width, 0), 1),
                            fmin(fmax((inView.y - viewport.origin.y) / viewport.size.height, 0), 1));
    *size = viewport.size;
    UIView *hit = [view.window hitTest:[view.window convertPoint:inView fromView:view] withEvent:nil];
    *landsOnGuest = UIApplication.sharedApplication.applicationState == UIApplicationStateActive &&
        CGRectContainsPoint(viewport, inView) && [hit isDescendantOfView:view] &&
        !CGRectContainsPoint(gKeyboardFrame, inScreen) && !windowAboveTakesPoint(scene, view.window, inScreen);
    return YES;
}

static void addTouch(NSMutableSet *touches, uint8_t slot, NSInteger phase, CGPoint location) {
    id touch = ((id(*)(id, SEL, uint8_t, NSInteger, CGPoint, NSInteger, double))objc_msgSend)(
        ((id(*)(id, SEL))objc_msgSend)(objc_getClass("_VZTouch"), sel_registerName("alloc")),
        sel_registerName("initWithIndex:phase:location:swipeAim:timestamp:"),
        (uint8_t)(slot - 1), phase, location, (NSInteger)0, CACurrentMediaTime());
    if (touch) {
        [touches addObject:touch];
        [touch release];
    }
}

static uint8_t slotForPath(uint8_t path, uint32_t inUse) {
    for (uint8_t slot = 1; slot <= kSlots; slot++)
        if ((inUse >> slot & 1) && gContacts[slot].path == path) return slot;
    return 0;
}

static void sendTouches(NSSet *touches) {
    if (!touches.count) return;
    id event = ((id(*)(id, SEL, id))objc_msgSend)(
        ((id(*)(id, SEL))objc_msgSend)(objc_getClass("_VZMultiTouchEvent"), sel_registerName("alloc")),
        sel_registerName("initWithTouches:"), touches);
    if (!event) return;
    ((void(*)(id, SEL, id))objc_msgSend)(gTouchDevice, sel_registerName("sendMultiTouchEvents:"), @[event]);
    [event release];
}

static void handleDigitizerEvent(IOHIDEventRef event) {
    CFArrayRef fingers = IOHIDEventGetChildren(event);
    CFIndex count = fingers ? CFArrayGetCount(fingers) : 0;
    CFTimeInterval now = CACurrentMediaTime();
    NSMutableSet *touches = [NSMutableSet set];
    uint32_t down = 0, seenPaths = 0;
    CGSize size = CGSizeZero;
    for (CFIndex i = 0; i < count; i++) {
        IOHIDEventRef finger = (IOHIDEventRef)CFArrayGetValueAtIndex(fingers, i);
        if (IOHIDEventGetType(finger) != kDigitizerEvent) continue;
        // The Pencil keeps using the app's own input path.
        if (IOHIDEventGetIntegerValue(finger, FIELD(kFieldType)) == kTypeStylus) continue;
        // The event's identity field names the finger iPadOS guesses it is, and
        // two contacts can share it; the path index is unique.
        uint8_t path = (uint8_t)(IOHIDEventGetIntegerValue(finger, FIELD(kFieldPath)) % kPaths);
        BOOL touching = IOHIDEventGetIntegerValue(finger, FIELD(kFieldTouch)) != 0;
        BOOL inRange = IOHIDEventGetIntegerValue(finger, FIELD(kFieldRange)) != 0;
        if (touching || inRange) seenPaths |= 1u << path;
        if (gIgnoredPaths >> path & 1) continue;
        uint8_t slot = slotForPath(path, gSentSlots | down);
        BOOL wasDown = slot && (gSentSlots >> slot & 1);
        // The touch flag of a finger already down can drop for a frame or two
        // while it slides; keep it down, at its last position, while it is in range.
        if (!touching && !(inRange && wasDown)) continue;
        CGPoint location = CGPointZero;
        BOOL landsOnGuest = NO;
        if (!locateFinger(IOHIDEventGetFloatValue(finger, FIELD(kFieldX)),
                          IOHIDEventGetFloatValue(finger, FIELD(kFieldY)),
                          &location, &size, &landsOnGuest)) continue;
        if (!wasDown) {
            for (uint8_t candidate = 1; candidate <= kSlots && !slot; candidate++)
                if (!((gSentSlots | down) >> candidate & 1)) slot = candidate;
            if (!slot || !landsOnGuest) {
                gIgnoredPaths |= 1u << path;
                continue;
            }
            gContacts[slot].path = path;
            gContacts[slot].edge = nearestEdge(location, size);
            gContacts[slot].began = now;
            gContacts[slot].location = location;
        }
        if (touching) gContacts[slot].location = location;
        CGPoint sent = shapedLocation(&gContacts[slot], now, size);
        gContacts[slot].sent = sent;
        down |= 1u << slot;
        addTouch(touches, slot, wasDown ? kPhaseMoved : kPhaseBegan, sent);
    }
    for (uint8_t slot = 1; slot <= kSlots; slot++)
        if ((gSentSlots >> slot & 1) && !(down >> slot & 1))
            addTouch(touches, slot, kPhaseEnded, gContacts[slot].sent);
    gSentSlots = down;
    // A contact left to the app stays that way until the finger lifts.
    gIgnoredPaths &= seenPaths;
    sendTouches(touches);
}

static void relayCallback(void *target, void *refcon, IOHIDServiceClientRef service, IOHIDEventRef event) {
    (void)target; (void)refcon; (void)service;
    if (gTouchDevice && IOHIDEventGetType(event) == kDigitizerEvent) handleDigitizerEvent(event);
}

void VZTouchScreenRelayStart(id virtualMachine, VZTouchScreenViewportProvider viewportProvider) {
    if (gClient) return;
    id devices = [virtualMachine respondsToSelector:sel_registerName("_multiTouchDevices")]
        ? ((id(*)(id, SEL))objc_msgSend)(virtualMachine, sel_registerName("_multiTouchDevices"))
        : nil;
    // The touch screen is configured after any Magic Trackpad.
    gTouchDevice = [[devices lastObject] retain];
    if (!gTouchDevice) return;
    gClient = IOHIDEventSystemClientCreateWithType(NULL, kMonitorClient, NULL);
    if (!gClient) {
        [gTouchDevice release];
        gTouchDevice = nil;
        return;
    }
    gViewportProvider = viewportProvider;
    gSentSlots = 0;
    gIgnoredPaths = 0;
    gKeyboardFrame = CGRectNull;
    gKeyboardObserver = [[NSNotificationCenter.defaultCenter
        addObserverForName:UIKeyboardWillChangeFrameNotification object:nil queue:NSOperationQueue.mainQueue
                usingBlock:^(NSNotification *note) {
        CGRect frame = [note.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
        // A hidden keyboard ends off screen; a floating one reports an empty frame.
        gKeyboardFrame = CGRectIntersection(frame, UIScreen.mainScreen.bounds);
    }] retain];
    IOHIDEventSystemClientRegisterEventCallback(gClient, relayCallback, NULL, NULL);
    IOHIDEventSystemClientScheduleWithRunLoop(gClient, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
}

void VZTouchScreenRelayStop(void) {
    if (!gClient) return;
    IOHIDEventSystemClientUnregisterEventCallback(gClient, relayCallback, NULL, NULL);
    IOHIDEventSystemClientUnscheduleWithRunLoop(gClient, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    CFRelease(gClient);
    gClient = NULL;
    [gTouchDevice release];
    gTouchDevice = nil;
    gViewportProvider = NULL;
    [NSNotificationCenter.defaultCenter removeObserver:gKeyboardObserver];
    [gKeyboardObserver release];
    gKeyboardObserver = nil;
}

BOOL VZTouchScreenRelayActive(void) {
    return gClient != NULL;
}
