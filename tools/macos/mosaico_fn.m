// SPDX-License-Identifier: Apache-2.0
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/hidsystem/IOHIDLib.h>
#import <IOKit/hidsystem/IOHIDEventSystemClient.h>
#import <IOKit/hidsystem/IOHIDServiceClient.h>
#import <IOKit/hid/IOHIDUsageTables.h>
#import <signal.h>
#include <stdatomic.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static void stop_requested(int signal_number) { (void)signal_number; stopped = 1; }
static NSString *const mapKey = @"UserKeyMapping";
static NSString *const sourceKey = @"HIDKeyboardModifierMappingSrc";
static NSString *const destinationKey = @"HIDKeyboardModifierMappingDst";
static const uint64_t sourceUsage = 0x70000006d;
static const uint64_t fnUsage = 0xff00000003;
static const int64_t mosaicoKeyboardType = 40;
static const CGKeyCode keyCodeTab = 0x30;
static const CGKeyCode keyCodeReturn = 0x24;
static const CGKeyCode keyCodeEscape = 0x35;
static const CGKeyCode keyCodeSpace = 0x31;
static const CGKeyCode keyCodeL = 0x25;
static const CGKeyCode keyCodeF4 = 0x76;
static const CGKeyCode keyCodeFunction = 0x3F;
static const CGKeyCode keyCodeF18 = 0x4F;
static const uint64_t syntheticMagic = 0x4d4f534149434f31ULL;

typedef struct {
    bool enabled;
    CGKeyCode keyCode;
    CGEventFlags flags;
    uint32_t delayMs;
} prefix_executor_t;

static prefix_executor_t prefixExecutor;
static CFMachPortRef prefixTap;
static CFMachPortRef observationTap;
static CFRunLoopSourceRef observationSource;
static BOOL testMode;
static unsigned testPrefixes, testFnDowns, testFnUps;
static CFRunLoopSourceRef prefixSource;
static _Atomic bool fnDownTracked;

static dispatch_queue_t diagnosticQueue;
static NSString *diagnosticPath;
static _Atomic unsigned diagnosticPending;
static BOOL diagnosticEnabled;

static NSString *eventLogPath(void)
{
    return [NSHomeDirectory() stringByAppendingPathComponent:
            @"Library/Application Support/MosaicoMic/key-events.log"];
}

static void initializeDiagnostics(NSString *path)
{
    diagnosticPath = [path copy];
    diagnosticQueue = dispatch_queue_create("local.mosaico.diagnostics", DISPATCH_QUEUE_SERIAL);
    diagnosticEnabled = YES;
}

/* Only explicit control events are logged, never typed text. Disk I/O stays off the event tap. */
static void logControl(NSString *message)
{
    if (!diagnosticEnabled) return;
    if (atomic_fetch_add(&diagnosticPending, 1) >= 256) {
        atomic_fetch_sub(&diagnosticPending, 1);
        return;
    }
    NSDate *timestamp = [NSDate date];
    dispatch_async(diagnosticQueue, ^{
        @autoreleasepool {
            static NSMutableArray<NSString *> *lines;
            static NSDateFormatter *formatter;
            if (!lines) {
                lines = [NSMutableArray array];
                formatter = [NSDateFormatter new];
                formatter.dateFormat = @"yyyy-MM-dd HH:mm:ss.SSS";
            }
            NSString *bounded = message.length > 512 ? [message substringToIndex:512] : message;
            [lines addObject:[NSString stringWithFormat:@"%@  %@", [formatter stringFromDate:timestamp], bounded]];
            if (lines.count > 400) [lines removeObjectsInRange:NSMakeRange(0, lines.count - 400)];
            NSError *error = nil;
            BOOL ok = [[NSFileManager defaultManager] createDirectoryAtPath:diagnosticPath.stringByDeletingLastPathComponent
                withIntermediateDirectories:YES attributes:nil error:&error];
            if (ok) ok = [[lines componentsJoinedByString:@"\n"] writeToFile:diagnosticPath atomically:YES
                encoding:NSUTF8StringEncoding error:&error];
            if (!ok) NSLog(@"Mosaico diagnostic write failed: %@", error.localizedDescription);
        }
        atomic_fetch_sub(&diagnosticPending, 1);
    });
}

static id property(IOHIDServiceClientRef service, NSString *key)
{
    return CFBridgingRelease(IOHIDServiceClientCopyProperty(service, (__bridge CFStringRef)key));
}

static NSString *configuredBoardSerial(void)
{
    NSString *path = [NSHomeDirectory() stringByAppendingPathComponent:
        @"Library/Application Support/MosaicoMic/board-serial.txt"];
    NSString *value = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    NSString *serial = [[value stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
        lowercaseString];
    NSCharacterSet *nonHex = [NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"].invertedSet;
    return serial.length == 12 && [serial rangeOfCharacterFromSet:nonHex].location == NSNotFound ? serial : nil;
}

static BOOL target(IOHIDServiceClientRef service)
{
    NSString *expectedSerial = configuredBoardSerial();
    if (!expectedSerial) return NO;
    NSString *product = property(service, @"Product");
    NSString *serial = property(service, @"SerialNumber");
    NSString *transport = property(service, @"Transport");
    // Bluetooth event services may expose the serial only on their parent device.
    if (!serial) {
        CFTypeRef identifier = IOHIDServiceClientGetRegistryID(service);
        int64_t registryID = 0;
        if (identifier && CFGetTypeID(identifier) == CFNumberGetTypeID() &&
            CFNumberGetValue((CFNumberRef)identifier, kCFNumberSInt64Type, &registryID)) {
            io_registry_entry_t entry = IOServiceGetMatchingService(kIOMainPortDefault,
                                        IORegistryEntryIDMatching((uint64_t)registryID));
            if (entry) {
                serial = CFBridgingRelease(IORegistryEntrySearchCFProperty(entry,
                    kIOServicePlane, CFSTR("SerialNumber"), kCFAllocatorDefault,
                    kIORegistryIterateRecursively | kIORegistryIterateParents));
                IOObjectRelease(entry);
            }
        }
    }
    if (![serial isKindOfClass:[NSString class]]) return NO;
    NSString *normalized = [[serial lowercaseString] stringByReplacingOccurrencesOfString:@":" withString:@""];
    normalized = [normalized stringByReplacingOccurrencesOfString:@"-" withString:@""];
    return IOHIDServiceClientConformsTo(service, 1, 6) &&
           [product isEqualToString:@"Mosaico Mic"] &&
           [transport isEqualToString:@"Bluetooth"] &&
           [normalized isEqualToString:expectedSerial];
}

static int process(BOOL apply)
{
    IOHIDEventSystemClientRef client = IOHIDEventSystemClientCreateSimpleClient(kCFAllocatorDefault);
    if (!client) return 2;
    CFArrayRef services = IOHIDEventSystemClientCopyServices(client);
    unsigned found = 0, verified = 0;
    if (services) {
        for (CFIndex i = 0; i < CFArrayGetCount(services); i++) {
            IOHIDServiceClientRef service = (IOHIDServiceClientRef)CFArrayGetValueAtIndex(services, i);
            NSString *product = property(service, @"Product");
            if (!target(service)) {
                if ([product containsString:@"Mosaico"] && !apply) {
                    printf("candidate=%s serial=%s transport=%s\n", product.UTF8String,
                           [[property(service, @"SerialNumber") description] UTF8String],
                           [[property(service, @"Transport") description] UTF8String]);
                }
                continue;
            }
            found++;
            NSArray *current = property(service, mapKey);
            if (current && ![current isKindOfClass:[NSArray class]]) continue;
            BOOL mapped = NO, conflict = NO;
            for (NSDictionary *entry in current) {
                if ([entry[sourceKey] unsignedLongLongValue] == sourceUsage) {
                    mapped = [entry[destinationKey] unsignedLongLongValue] == fnUsage;
                    conflict = !mapped;
                }
            }
            if (!mapped && !conflict && apply) {
                NSMutableArray *updated = current ? [current mutableCopy] : [NSMutableArray array];
                [updated addObject:@{sourceKey:@(sourceUsage), destinationKey:@(fnUsage)}];
                if (IOHIDServiceClientSetProperty(service, (__bridge CFStringRef)mapKey,
                                                 (__bridge CFArrayRef)updated)) {
                    mapped = [property(service, mapKey) isEqual:updated];
                }
            }
            if (mapped) verified++;
            if (!apply || !mapped) {
                printf("Mosaico mapping=%s conflict=%d\n", mapped ? "verified" : "not_set", conflict);
            }
        }
        CFRelease(services);
    }
    CFRelease(client);
    if (!apply || found) printf("{\"matched\":%u,\"mapping_verified\":%u}\n", found, verified);
    return found && verified == found ? 0 : found ? 2 : 3;
}

static NSArray<NSString *> *prefixKeys(void)
{
    return @[@"None", @"Tab", @"Return", @"Escape", @"Space", @"L", @"F4"];
}

static NSString *keyTitle(NSString *key)
{
    NSDictionary *titles = @{@"None": @"无前置按键", @"Tab": @"Tab（制表键）",
        @"Return": @"Return（回车）", @"Escape": @"Esc（退出键）", @"Space": @"Space（空格）",
        @"Command": @"Command（命令）", @"Option": @"Option（选项）",
        @"Control": @"Control（控制）", @"Shift": @"Shift（换挡）"};
    return titles[key] ?: key;
}

static NSDictionary *defaultDraft(void)
{
    return @{@"schema": @1, @"prefixKey": @"L", @"modifiers": @[@"Command", @"Option", @"Control"],
             @"delayMs": @100, @"executionEnabled": @YES};
}

static BOOL validDraft(id value)
{
    if (![value isKindOfClass:NSDictionary.class]) return NO;
    NSDictionary *d = value;
    if (![d[@"schema"] isEqual:@1] || ![prefixKeys() containsObject:d[@"prefixKey"]] ||
        ![d[@"executionEnabled"] isKindOfClass:NSNumber.class] ||
        ![d[@"delayMs"] isKindOfClass:NSNumber.class] ||
        ![d[@"modifiers"] isKindOfClass:NSArray.class]) return NO;
    double delay = [d[@"delayMs"] doubleValue];
    if (!isfinite(delay) || delay < 0 || delay > 2000 || floor(delay) != delay) return NO;
    NSArray *allowed = @[@"Command", @"Option", @"Control", @"Shift"];
    NSMutableSet *seen = [NSMutableSet set];
    for (id modifier in d[@"modifiers"]) {
        if (![allowed containsObject:modifier] || [seen containsObject:modifier]) return NO;
        [seen addObject:modifier];
    }
    return YES;
}


static NSDictionary<NSString *, NSNumber *> *prefixKeyCodes(void)
{
    return @{@"Tab": @(keyCodeTab), @"Return": @(keyCodeReturn), @"Escape": @(keyCodeEscape),
             @"Space": @(keyCodeSpace), @"L": @(keyCodeL), @"F4": @(keyCodeF4)};
}

static NSDictionary<NSString *, NSNumber *> *modifierFlags(void)
{
    return @{@"Command": @(kCGEventFlagMaskCommand), @"Option": @(kCGEventFlagMaskAlternate),
             @"Control": @(kCGEventFlagMaskControl), @"Shift": @(kCGEventFlagMaskShift)};
}

static void requestSystemAccess(void)
{
    NSDictionary *options = @{(__bridge NSString *)kAXTrustedCheckOptionPrompt: @YES};
    (void)AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
    (void)IOHIDRequestAccess(kIOHIDRequestTypePostEvent);
    (void)IOHIDRequestAccess(kIOHIDRequestTypeListenEvent);
}

static NSDictionary *loadPrefixDraft(NSString **message)
{
    NSURL *url = [NSURL fileURLWithPath:[NSHomeDirectory()
        stringByAppendingPathComponent:@"Library/Application Support/MosaicoMic/prefix-draft.json"]];
    NSError *error = nil;
    NSData *data = [NSData dataWithContentsOfURL:url options:0 error:&error];
    if (!data) {
        if (message) *message = error.localizedDescription;
        return nil;
    }
    id value = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
    if (!validDraft(value)) {
        if (message) *message = @"配置文件格式无效";
        return nil;
    }
    return value;
}

static dispatch_queue_t prefixQueue;
static _Atomic uint64_t prefixGeneration;
static _Atomic bool syntheticFnDown;

static void postFnState(bool down)
{
    if (testMode) { if (down) testFnDowns++; else testFnUps++; return; }
    CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStatePrivate);
    if (!source) { logControl(@"发送 Fn 失败：无法创建事件源"); return; }
    CGEventRef event = CGEventCreateKeyboardEvent(source, (CGKeyCode)keyCodeFunction, down);
    if (event) {
        CGEventSetType(event, kCGEventFlagsChanged);
        CGEventSetFlags(event, down ? kCGEventFlagMaskSecondaryFn : 0);
        CGEventSetIntegerValueField(event, kCGEventSourceUserData, (int64_t)syntheticMagic);
        CGEventPost(kCGHIDEventTap, event);
        logControl(down ? @"已向系统投递 Fn 按下（不代表语音工具已开始录音）" : @"已向系统投递 Fn 松开");
        CFRelease(event);
    } else logControl(@"发送 Fn 失败：无法创建键盘事件");
    CFRelease(source);
}

static void releaseSyntheticFn(void)
{
    if (atomic_exchange(&syntheticFnDown, false)) postFnState(false);
}

static void postPrefix(void)
{
    if (testMode) { testPrefixes++; return; }
    CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStatePrivate);
    if (!source) { logControl(@"前置键发送失败：无法创建事件源"); return; }
    CGEventRef down = CGEventCreateKeyboardEvent(source, prefixExecutor.keyCode, true);
    CGEventRef up = CGEventCreateKeyboardEvent(source, prefixExecutor.keyCode, false);
    logControl([NSString stringWithFormat:@"投递前置组合键：keyCode=0x%02x，修饰标志=0x%llx",
                prefixExecutor.keyCode, (unsigned long long)prefixExecutor.flags]);
    if (!down || !up) logControl(@"前置键发送不完整：键盘事件创建失败");
    if (down) {
        CGEventSetFlags(down, prefixExecutor.flags);
        CGEventSetIntegerValueField(down, kCGEventSourceUserData, (int64_t)syntheticMagic);
        CGEventPost(kCGHIDEventTap, down);
        CFRelease(down);
    }
    if (up) {
        CGEventSetFlags(up, prefixExecutor.flags);
        CGEventSetIntegerValueField(up, kCGEventSourceUserData, (int64_t)syntheticMagic);
        CGEventPost(kCGHIDEventTap, up);
        CFRelease(up);
    }
    CFRelease(source);
}

static CGEventRef prefix_event_callback(CGEventTapProxy proxy, CGEventType type,
                                        CGEventRef event, void *context)
{
    (void)proxy; (void)context;
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        logControl(@"事件监听被系统停用；取消延时、释放 Fn 并重新启用监听");
        atomic_fetch_add(&prefixGeneration, 1);
        atomic_store(&fnDownTracked, false);
        releaseSyntheticFn();
        if (prefixTap) CGEventTapEnable(prefixTap, true);
        return event;
    }
    if (!prefixExecutor.enabled) return event;
    const int64_t userData = CGEventGetIntegerValueField(event, kCGEventSourceUserData);
    const int64_t sourceState = CGEventGetIntegerValueField(event, kCGEventSourceStateID);
    const int64_t keyboardType = CGEventGetIntegerValueField(event, kCGKeyboardEventKeyboardType);
    const int64_t keyCode = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
    const bool controlCandidate = keyCode == keyCodeFunction || keyCode == keyCodeF18;
    const bool repeatEvent = CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat) != 0;
    if (controlCandidate && !repeatEvent && userData != (int64_t)syntheticMagic) {
        logControl([NSString stringWithFormat:@"收到 %@：事件=%u，键盘类型=%lld，来源=%lld，Fn标志=%d",
                    keyCode == keyCodeF18 ? @"F18" : @"Fn", (unsigned)type,
                    (long long)keyboardType, (long long)sourceState,
                    (CGEventGetFlags(event) & kCGEventFlagMaskSecondaryFn) != 0]);
    }
    if (userData == (int64_t)syntheticMagic ||
            (sourceState != kCGEventSourceStateHIDSystemState && sourceState != kCGEventSourceStateCombinedSessionState) ||
            (keyboardType != mosaicoKeyboardType && !(keyboardType == 0 && sourceState == kCGEventSourceStateHIDSystemState))) {
        if (controlCandidate && !repeatEvent && userData != (int64_t)syntheticMagic)
            logControl(@"忽略此 F18/Fn：来源或键盘类型未通过过滤（类型不是设备唯一标识）");
        return event;
    }
    const bool isFnKey = keyCode == keyCodeFunction || keyCode == keyCodeF18;
    if (!isFnKey) return event;
    const bool flagsEvent = type == kCGEventFlagsChanged && keyCode == keyCodeFunction;
    const bool down = type == kCGEventKeyDown ||
                      (flagsEvent && (CGEventGetFlags(event) & kCGEventFlagMaskSecondaryFn));
    const bool up = type == kCGEventKeyUp || (flagsEvent && !down);
    if (up && atomic_exchange(&fnDownTracked, false)) {
        logControl(@"收到松开：清除按住状态、取消延时并释放 Fn；下一次按下可重新触发");
        atomic_fetch_add(&prefixGeneration, 1);
        releaseSyntheticFn();
        return NULL;
    }
    if (down) {
        if (!repeatEvent && !atomic_exchange(&fnDownTracked, true)) {
            const uint64_t generation = atomic_fetch_add(&prefixGeneration, 1) + 1;
            postPrefix();
            const uint32_t delay = MAX(prefixExecutor.delayMs, 20);
            logControl([NSString stringWithFormat:@"拦截原按键；等待 %u 毫秒后投递 Fn 按下", delay]);
            // Keep transitions, config updates and delayed actions on the event-tap run loop.
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)delay * NSEC_PER_MSEC),
                           dispatch_get_main_queue(), ^{
                if (atomic_load(&prefixGeneration) == generation && prefixExecutor.enabled && fnDownTracked) {
                    postFnState(true);
                    atomic_store(&syntheticFnDown, true);
                } else logControl(@"延时 Fn 已取消：按键已松开或配置已变更");
            });
        }
        if (fnDownTracked) return NULL;
    }
    return event;
}


static void writeRuntimeState(NSString *state)
{
    static NSString *previous;
    if (![previous isEqual:state]) {
        logControl([NSString stringWithFormat:@"守护进程状态：%@；辅助功能权限=%d", state, AXIsProcessTrusted()]);
        previous = [state copy];
    }
    NSURL *url = [NSURL fileURLWithPath:[NSHomeDirectory()
        stringByAppendingPathComponent:@"Library/Application Support/MosaicoMic/runtime-status.json"]];
    NSDictionary *value = @{@"state": state, @"pid": @(getpid()),
                            @"accessibility": @(AXIsProcessTrusted()),
                            @"updatedAt": [[NSISO8601DateFormatter new] stringFromDate:[NSDate date]]};
    NSData *data = [NSJSONSerialization dataWithJSONObject:value options:NSJSONWritingPrettyPrinted error:nil];
    [data writeToURL:url options:NSDataWritingAtomic error:nil];
}

static CGEventRef observePostedEvent(CGEventTapProxy proxy, CGEventType type,
                                     CGEventRef event, void *context)
{
    (void)proxy; (void)context;
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        if (observationTap) CGEventTapEnable(observationTap, true);
        logControl(@"系统回读监听被停用，已请求恢复");
        return event;
    }
    if (CGEventGetIntegerValueField(event, kCGEventSourceUserData) == (int64_t)syntheticMagic) {
        const int64_t code = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
        logControl([NSString stringWithFormat:@"系统事件流已观察到：%@，事件=%u，keyCode=0x%02llx，flags=0x%llx（不等于应用已处理）",
                    code == keyCodeFunction ? @"Fn" : @"前置组合键", (unsigned)type,
                    (unsigned long long)code, (unsigned long long)CGEventGetFlags(event)]);
    }
    return event;
}

static BOOL installPrefixTap(void)
{
    static BOOL reported;
    if (prefixTap) return YES;
    if (!AXIsProcessTrusted()) {
        static BOOL prompted;
        if (!prompted) {
            NSDictionary *options = @{(__bridge NSString *)kAXTrustedCheckOptionPrompt: @YES};
            (void)AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
            (void)IOHIDRequestAccess(kIOHIDRequestTypeListenEvent);
            prompted = YES;
        }
        if (!reported) { NSLog(@"Mosaico prefix: waiting for Accessibility permission"); reported = YES; }
        writeRuntimeState(@"permission_required");
        return NO;
    }
    if (!prefixQueue) prefixQueue = dispatch_queue_create("local.mosaico.prefix", DISPATCH_QUEUE_SERIAL);
    const CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp) |
                             CGEventMaskBit(kCGEventFlagsChanged);
    prefixTap = CGEventTapCreate(kCGHIDEventTap, kCGHeadInsertEventTap,
                                 kCGEventTapOptionDefault, mask, prefix_event_callback, NULL);
    if (!prefixTap) { writeRuntimeState(@"event_tap_failed"); return NO; }
    prefixSource = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, prefixTap, 0);
    if (!prefixSource) {
        CFRelease(prefixTap); prefixTap = NULL; return NO;
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), prefixSource, kCFRunLoopCommonModes);
    CGEventTapEnable(prefixTap, true);
    observationTap = CGEventTapCreate(kCGSessionEventTap, kCGTailAppendEventTap,
        kCGEventTapOptionListenOnly, mask, observePostedEvent, NULL);
    if (observationTap) {
        observationSource = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, observationTap, 0);
        if (observationSource) {
            CFRunLoopAddSource(CFRunLoopGetCurrent(), observationSource, kCFRunLoopCommonModes);
            CGEventTapEnable(observationTap, true);
            logControl(@"系统事件回读已启用：只观察本程序标记的合成事件");
        } else {
            CFRelease(observationTap); observationTap = NULL;
            logControl(@"系统事件回读初始化失败，不能验证投递结果");
        }
    } else logControl(@"系统事件回读不可用：请检查输入监控权限");
    NSLog(@"Mosaico prefix: event tap active");
    writeRuntimeState(@"event_tap_active");
    return YES;
}

static BOOL configurePrefixExecutor(NSDictionary *draft)
{
    static NSDictionary *appliedDraft;
    if (draft && [draft isEqual:appliedDraft])
        return prefixExecutor.enabled ? installPrefixTap() : YES;
    appliedDraft = [draft copy];
    atomic_fetch_add(&prefixGeneration, 1);
    atomic_store(&fnDownTracked, false);
    releaseSyntheticFn();
    prefixExecutor = (prefix_executor_t){0};
    if (![draft[@"executionEnabled"] boolValue] || [draft[@"prefixKey"] isEqual:@"None"]) {
        atomic_fetch_add(&prefixGeneration, 1);
        releaseSyntheticFn();
        writeRuntimeState(@"disabled");
        return YES;
    }
    NSNumber *code = prefixKeyCodes()[draft[@"prefixKey"]];
    if (!code) return NO;
    CGEventFlags flags = 0;
    for (NSString *name in draft[@"modifiers"]) flags |= (CGEventFlags)modifierFlags()[name].unsignedLongLongValue;
    prefixExecutor = (prefix_executor_t){.enabled = true, .keyCode = (CGKeyCode)code.unsignedShortValue,
                                          .flags = flags, .delayMs = [draft[@"delayMs"] unsignedIntValue]};
    return installPrefixTap();
}

@interface MappingSettings : NSObject <NSApplicationDelegate>
@property NSWindow *window;
@property NSPopUpButton *key;
@property NSArray<NSButton *> *modifiers;
@property NSSlider *delay;
@property NSTextField *delayLabel;
@property NSTextField *preview;
@property NSTextField *message;
@property NSURL *draftURL;
@property BOOL canSave;
@property NSTextView *logView;
@property NSTimer *logTimer;
@end

@implementation MappingSettings
- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
    (void)notification;
    self.canSave = YES;
    self.draftURL = [NSURL fileURLWithPath:[NSHomeDirectory()
        stringByAppendingPathComponent:@"Library/Application Support/MosaicoMic/prefix-draft.json"]];
    NSDictionary *draft = defaultDraft();
    NSError *error = nil;
    NSData *data = [NSData dataWithContentsOfURL:self.draftURL options:0 error:&error];
    NSString *loadMessage = AXIsProcessTrusted() ? @"前置按键发送已就绪。" : @"尚未授予辅助功能权限，发送不会启动。";
    if (data) {
        id saved = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
        if (validDraft(saved)) draft = saved;
        else { self.canSave = NO; loadMessage = @"已有草稿格式无效，已禁止覆盖，原文件保留。"; }
    } else if (error && !([error.domain isEqualToString:NSCocoaErrorDomain] &&
                          error.code == NSFileReadNoSuchFileError)) {
        self.canSave = NO;
        loadMessage = error.localizedDescription;
    }
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 660, 760)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"Mosaico 无线麦 · 前置按键设置";
    self.window.releasedWhenClosed = NO;
    NSView *view = self.window.contentView;
    NSTextField *title = [NSTextField labelWithString:@"前置按键设置"];
    title.font = [NSFont boldSystemFontOfSize:24];
    title.frame = NSMakeRect(28, 450, 604, 32); [view addSubview:title];
    NSDictionary *info = NSBundle.mainBundle.infoDictionary;
    NSTextField *version = [NSTextField labelWithString:[NSString stringWithFormat:@"v%@ (%@)",
        info[@"CFBundleShortVersionString"] ?: @"?", info[@"CFBundleVersion"] ?: @"?"]];
    version.frame = NSMakeRect(450, 454, 182, 22);
    version.alignment = NSTextAlignmentRight;
    version.textColor = NSColor.secondaryLabelColor;
    [version setAccessibilityLabel:@"应用版本号"];
    [view addSubview:version];
    NSTextField *subtitle = [NSTextField labelWithString:@"F18 触发  /  前置按键或组合键  /  按住 Fn"];
    subtitle.frame = NSMakeRect(28, 416, 604, 24); [view addSubview:subtitle];
    self.key = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(28, 366, 230, 32) pullsDown:NO];
    // Display localized labels without changing the existing JSON schema.
    for (NSString *key in prefixKeys()) {
        [self.key addItemWithTitle:keyTitle(key)];
        self.key.lastItem.representedObject = key;
        if ([key isEqual:draft[@"prefixKey"]]) [self.key selectItem:self.key.lastItem];
    }
    self.key.target = self; self.key.action = @selector(changed:);
    [self.key setAccessibilityLabel:@"前置按键"]; [view addSubview:self.key];
    NSMutableArray *buttons = [NSMutableArray array];
    NSArray *names = @[@"Command", @"Option", @"Control", @"Shift"];
    for (NSUInteger i = 0; i < names.count; i++) {
        NSButton *button = [NSButton checkboxWithTitle:keyTitle(names[i]) target:self action:@selector(changed:)];
        button.identifier = names[i];
        button.frame = NSMakeRect(28 + i * 152, 318, 150, 28);
        button.state = [draft[@"modifiers"] containsObject:names[i]] ? NSControlStateValueOn : NSControlStateValueOff;
        [buttons addObject:button]; [view addSubview:button];
    }
    self.modifiers = buttons;
    self.delayLabel = [NSTextField labelWithString:@""];
    self.delayLabel.frame = NSMakeRect(28, 278, 604, 24); [view addSubview:self.delayLabel];
    self.delay = [NSSlider sliderWithValue:[draft[@"delayMs"] doubleValue] minValue:0 maxValue:2000
                                  target:self action:@selector(changed:)];
    self.delay.frame = NSMakeRect(28, 245, 604, 26);
    [self.delay setAccessibilityLabel:@"前置键松开到 Fn 按下的间隔，毫秒"]; [view addSubview:self.delay];
    self.preview = [NSTextField wrappingLabelWithString:@""];
    self.preview.frame = NSMakeRect(28, 139, 604, 90); [view addSubview:self.preview];
    self.message = [NSTextField wrappingLabelWithString:loadMessage];
    self.message.textColor = NSColor.secondaryLabelColor;
    self.message.frame = NSMakeRect(28, 64, 604, 58); [view addSubview:self.message];
    NSButton *disable = [NSButton buttonWithTitle:@"停用前置键" target:self action:@selector(disablePrefix:)];
    disable.frame = NSMakeRect(302, 20, 120, 32); disable.enabled = self.canSave; [view addSubview:disable];
    NSButton *enable = [NSButton buttonWithTitle:@"应用并启用" target:self action:@selector(enablePrefix:)];
    enable.frame = NSMakeRect(437, 20, 195, 32); enable.enabled = self.canSave; [view addSubview:enable];
    NSButton *permission = [NSButton buttonWithTitle:@"申请权限" target:self action:@selector(requestPermissions:)];
    permission.frame = NSMakeRect(28, 20, 110, 32); [view addSubview:permission];
    for (NSView *control in view.subviews) {
        NSRect frame = control.frame;
        frame.origin.y += 250;
        control.frame = frame;
    }
    NSTextField *logTitle = [NSTextField labelWithString:@"运行日志 · 仅记录 F18 / Fn 和前置键流程，不记录文字"];
    logTitle.frame = NSMakeRect(28, 218, 604, 24); [view addSubview:logTitle];
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(28, 24, 604, 185)];
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSBezelBorder;
    self.logView = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 580, 185)];
    self.logView.editable = NO;
    self.logView.selectable = YES;
    self.logView.richText = NO;
    self.logView.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    self.logView.verticallyResizable = YES;
    self.logView.autoresizingMask = NSViewWidthSizable;
    self.logView.textContainer.widthTracksTextView = YES;
    [self.logView setAccessibilityLabel:@"守护进程实时运行日志"];
    scroll.documentView = self.logView;
    [view addSubview:scroll];
    [self refreshLogs:nil];
    self.logTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 target:self
        selector:@selector(refreshLogs:) userInfo:nil repeats:YES];
    [self changed:nil]; [self.window center]; [self.window makeKeyAndOrderFront:nil];
    [NSApp activate];
}
- (void)refreshLogs:(NSTimer *)timer
{
    (void)timer;
    NSError *error = nil;
    NSString *text = [NSString stringWithContentsOfFile:eventLogPath() encoding:NSUTF8StringEncoding error:&error];
    if (!text) text = error.code == NSFileReadNoSuchFileError ?
        @"等待守护进程日志。若持续空白，请确认新版服务已启动并获得权限。" :
        [NSString stringWithFormat:@"日志读取失败：%@", error.localizedDescription];
    if (![self.logView.string isEqual:text]) {
        BOOL atBottom = NSMaxY(self.logView.visibleRect) >= NSMaxY(self.logView.bounds) - 24;
        self.logView.string = text;
        if (atBottom) [self.logView scrollRangeToVisible:NSMakeRange(text.length, 0)];
    }
}
- (NSDictionary *)draftWithEnabled:(BOOL)enabled
{
    NSMutableArray *modifiers = [NSMutableArray array];
    for (NSButton *button in self.modifiers)
        if (button.state == NSControlStateValueOn) [modifiers addObject:button.identifier];
    return @{@"schema": @1, @"prefixKey": self.key.selectedItem.representedObject,
             @"modifiers": modifiers, @"delayMs": @(lround(self.delay.doubleValue)),
             @"executionEnabled": @(enabled)};
}
- (void)changed:(id)sender
{
    (void)sender;
    NSDictionary *d = [self draftWithEnabled:NO];
    BOOL none = [d[@"prefixKey"] isEqual:@"None"];
    for (NSButton *button in self.modifiers) button.enabled = !none;
    self.delay.enabled = !none;
    self.delayLabel.stringValue = [NSString stringWithFormat:@"前置键后等待：%@ 毫秒", d[@"delayMs"]];
    NSMutableArray *keys = [d[@"modifiers"] mutableCopy]; [keys addObject:keyTitle(d[@"prefixKey"])];
    self.preview.stringValue = none ? @"顺序预览：按下 F18 → 按住 Fn；松开 F18 → 松开 Fn。" :
        [NSString stringWithFormat:@"顺序预览：轻按并松开 %@ → 等待 %@ 毫秒 → 按住 Fn。\n松开 F18 → 松开 Fn。\n%@",
            [keys componentsJoinedByString:@" + "], d[@"delayMs"],
            [d[@"prefixKey"] isEqual:@"Return"] ? @"注意：回车可能提交尚未完成的文字。" :
            @"注意：前置键会作用于当前前台应用。"];
}
- (BOOL)writeDraft:(NSDictionary *)draft message:(NSString **)message
{
    if (!self.canSave || !validDraft(draft)) return NO;
    NSError *error = nil;
    NSData *data = [NSJSONSerialization dataWithJSONObject:draft options:NSJSONWritingPrettyPrinted error:&error];
    BOOL ok = data && [[NSFileManager defaultManager] createDirectoryAtURL:self.draftURL.URLByDeletingLastPathComponent
        withIntermediateDirectories:YES attributes:nil error:&error];
    if (ok) ok = [data writeToURL:self.draftURL options:NSDataWritingAtomic error:&error];
    if (!ok && message) *message = [NSString stringWithFormat:@"保存失败：%@", error.localizedDescription];
    return ok;
}
- (void)enablePrefix:(id)sender
{
    (void)sender;
    NSString *message = nil;
    NSDictionary *draft = [self draftWithEnabled:YES];
    if (![self writeDraft:draft message:&message]) { self.message.stringValue = message; return; }
    if (!AXIsProcessTrusted()) {
        requestSystemAccess();
        self.message.stringValue = @"配置已保存，但还需要在“系统设置 → 隐私与安全性 → 辅助功能/输入监控”允许本应用。授权后点“应用并启用”。";
    } else {
        self.message.stringValue = @"已启用。后台服务会在约 2 秒内读取配置；按住 F18 时先发送前置键，再触发 Fn。";
    }
}
- (void)disablePrefix:(id)sender
{
    (void)sender;
    NSString *message = nil;
    if (![self writeDraft:[self draftWithEnabled:NO] message:&message]) {
        self.message.stringValue = message; return;
    }
    self.message.stringValue = @"前置键发送已停用；F18 → Fn 原有映射保持。";
}
- (void)requestPermissions:(id)sender
{
    (void)sender;
    requestSystemAccess();
    self.message.stringValue = @"已请求辅助功能和输入监控权限。请在系统设置中允许本应用，然后点“应用并启用”。";
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
    (void)sender; return YES;
}
@end

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        if (argc == 2 && strcmp(argv[1], "--test-events") == 0) {
            testMode = YES;
            prefixExecutor = (prefix_executor_t){.enabled = true, .delayMs = 20};
            CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
            if (!source) return 1;
            CGEventRef event = CGEventCreateKeyboardEvent(source, keyCodeFunction, true);
            CFRelease(source);
            if (!event) return 1;
            CGEventSetIntegerValueField(event, kCGKeyboardEventKeyboardType, mosaicoKeyboardType);
            for (unsigned i = 0; i < 2; ++i) {
                CGEventSetFlags(event, kCGEventFlagMaskSecondaryFn);
                prefix_event_callback(NULL, kCGEventFlagsChanged, event, NULL);
                [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.08]];
                CGEventSetFlags(event, 0);
                prefix_event_callback(NULL, kCGEventFlagsChanged, event, NULL);
            }
            BOOL ok = testPrefixes == 2 && testFnDowns == 2 && testFnUps == 2 && !fnDownTracked;
            CGEventSetFlags(event, kCGEventFlagMaskSecondaryFn);
            prefix_event_callback(NULL, kCGEventFlagsChanged, event, NULL);
            CGEventSetFlags(event, 0);
            prefix_event_callback(NULL, kCGEventFlagsChanged, event, NULL);
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.08]];
            ok = ok && testPrefixes == 3 && testFnDowns == 2 && testFnUps == 2 && !syntheticFnDown;
            CGEventSetIntegerValueField(event, kCGKeyboardEventKeycode, keyCodeF18);
            prefix_event_callback(NULL, kCGEventKeyDown, event, NULL);
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.08]];
            prefix_event_callback(NULL, kCGEventKeyUp, event, NULL);
            ok = ok && testPrefixes == 4 && testFnDowns == 3 && testFnUps == 3 && !fnDownTracked;
            CFRelease(event);
            puts(ok ? "Fn repeat cycles, short hold cancellation, F18 release: PASS" : "event tests: FAIL");
            return ok ? 0 : 1;
        }
        if (argc == 2 && strcmp(argv[1], "--test-log") == 0) {
            NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:
                              [NSString stringWithFormat:@"mosaico-log-%@.txt", NSUUID.UUID.UUIDString]];
            initializeDiagnostics(path);
            for (unsigned i = 0; i < 405; ++i) {
                logControl([NSString stringWithFormat:@"测试日志 %u", i]);
                dispatch_sync(diagnosticQueue, ^{});
            }
            NSString *text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
            BOOL ok = [text componentsSeparatedByString:@"\n"].count == 400 &&
                      [text containsString:@"测试日志 404"] && ![text containsString:@"测试日志 0\n"];
            [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
            puts(ok ? "bounded diagnostic log: PASS" : "bounded diagnostic log: FAIL");
            return ok ? 0 : 1;
        }
        if (argc == 2 && strcmp(argv[1], "--request-access") == 0) {
            requestSystemAccess();
            printf("AXTrusted=%d postEvent=%d listenEvent=%d\n", AXIsProcessTrusted(),
                   IOHIDCheckAccess(kIOHIDRequestTypePostEvent) == kIOHIDAccessTypeGranted,
                   IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) == kIOHIDAccessTypeGranted);
            return AXIsProcessTrusted() ? 0 : 2;
        }
        if (argc == 1 || (argc == 2 && strcmp(argv[1], "--settings") == 0)) {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            MappingSettings *settings = [MappingSettings new];
            NSApp.delegate = settings;
            [NSApp run];
            return 0;
        }
        if (argc == 2 && strcmp(argv[1], "--test-config") == 0) {
            if (!validDraft(defaultDraft()) || validDraft(@[]) || validDraft(@{})) return 1;
            for (NSString *key in prefixKeys()) {
                NSMutableDictionary *d = [defaultDraft() mutableCopy]; d[@"prefixKey"] = key;
                if (!validDraft(d)) return 1;
            }
            for (NSDictionary *bad in @[@{@"delayMs": @(-1)}, @{@"delayMs": @2001},
                @{@"delayMs": @1.5}, @{@"prefixKey": @"Unknown"}, @{@"prefixKey": @"F18"},
                @{@"modifiers": @[@"Command", @"Command"]}, @{@"modifiers": @[@"Unknown"]}]) {
                NSMutableDictionary *d = [defaultDraft() mutableCopy]; [d addEntriesFromDictionary:bad];
                if (validDraft(d)) return 1;
            }
            NSMutableDictionary *enabled = [defaultDraft() mutableCopy];
            enabled[@"executionEnabled"] = @YES;
            enabled[@"prefixKey"] = @"Tab";
            if (!validDraft(enabled)) return 1;
            for (NSString *key in prefixKeyCodes()) if (!prefixKeyCodes()[key]) return 1;
            if (prefixKeyCodes()[@"F18"]) return 1;
            NSData *data = [NSJSONSerialization dataWithJSONObject:enabled options:0 error:nil];
            if (!validDraft([NSJSONSerialization JSONObjectWithData:data options:0 error:nil])) return 1;
            puts("config validation and JSON round-trip: PASS"); return 0;
        }
        BOOL watch = argc == 2 && strcmp(argv[1], "--watch") == 0;
        BOOL apply = watch || (argc == 2 && strcmp(argv[1], "--apply") == 0);
        if (watch) {
            initializeDiagnostics(eventLogPath());
            logControl(@"守护进程启动；日志最多保留本次运行最近 400 行；投递事件不等于应用已响应");
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        }
        if (argc != 2 || (!apply && strcmp(argv[1], "--status") != 0)) {
            fputs("usage: mosaico-fn --settings|--request-access|--status|--apply|--watch|--test-config\n", stderr); return 1;
        }
        setbuf(stdout, NULL);
        signal(SIGTERM, stop_requested); signal(SIGINT, stop_requested);
        int result;
        int previousResult = -1;
        NSDictionary *previousDraft = nil;
        do {
            @autoreleasepool {
                result = process(apply);
                if (watch) {
                    if (result != 0 && previousResult == 0) {
                        atomic_fetch_add(&prefixGeneration, 1);
                        atomic_store(&fnDownTracked, false);
                        releaseSyntheticFn();
                    }
                    if (result != previousResult) {
                        logControl(result == 0 ? @"Mosaico 键盘已找到，F18 → Fn 映射已核验" :
                                   result == 3 ? @"未找到 Mosaico 蓝牙键盘；等待连接" : @"键盘映射失败或存在冲突");
                        previousResult = result;
                    }
                    NSString *message = nil;
                    NSDictionary *draft = loadPrefixDraft(&message);
                    if (![draft isEqual:previousDraft]) {
                        logControl(draft ? [NSString stringWithFormat:@"读取配置：前置键 %@ + %@；等待 %@ 毫秒；启用=%@",
                            [draft[@"modifiers"] componentsJoinedByString:@" + "], draft[@"prefixKey"],
                            draft[@"delayMs"], draft[@"executionEnabled"]] : @"配置不可用，暂停前置按键");
                        previousDraft = [draft copy];
                    }
                    configurePrefixExecutor(draft);
                }
            }
            if (watch && !stopped) {
                [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:2.0]];
            }
        } while (watch && !stopped);
        if (watch) {
            atomic_fetch_add(&prefixGeneration, 1);
            releaseSyntheticFn();
            if (diagnosticQueue) dispatch_sync(diagnosticQueue, ^{});
        }
        return watch ? 0 : result;
    }
}
