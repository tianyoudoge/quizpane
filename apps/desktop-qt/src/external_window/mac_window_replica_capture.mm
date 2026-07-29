#include "mac_window_replica_controller.hpp"
#include "quizpane/diagnostic_logger.hpp"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <QMetaObject>
#include <QDebug>

#include <algorithm>
#include <cmath>

namespace quizpane::external_window {

namespace {

constexpr int kMaxCaptureWidth = 1920;
constexpr int kMaxCaptureHeight = 1080;
// 这是当前扩展过去试图保留的标题栏面积。Chrome 扩展 API 会拒绝这个位置，
// 而 AX 只在诊断包中作为一次受控验证使用：窗口仍是 normal，且至少有一角
// 落在屏幕内，随后用帧流健康检查决定是否保留这个位置。
constexpr CGFloat kAccessibilityParkExposeWidth = 70.0;
constexpr CGFloat kAccessibilityParkExposeHeight = 28.0;
constexpr NSUInteger kAccessibilityParkMinimumFreshFrames = 10;

AttachResult failureResult(const AttachRequest& request, const QString& error,
                           AttachError errorCode = AttachError::CaptureFailed) {
    AttachResult result;
    result.sessionId = request.sessionId;
    result.backend = BackendType::MacCapturedReplica;
    result.capabilities = {false, true, true, true, true};
    result.errorCode = errorCode;
    result.error = error;
    return result;
}

bool supportedBrowserBundle(NSString* bundleIdentifier) {
    if (bundleIdentifier == nil) return false;
    static NSSet<NSString*>* const supported = [NSSet setWithArray:@[
        @"com.google.Chrome",
        @"com.google.Chrome.canary",
        @"org.chromium.Chromium",
        @"com.microsoft.edgemac",
    ]];
    return [supported containsObject:bundleIdentifier];
}

NSDictionary* windowInfoForId(CGWindowID windowId) {
    if (windowId == kCGNullWindowID) return nil;
    CFArrayRef rawWindows = CGWindowListCopyWindowInfo(kCGWindowListOptionAll,
                                                        kCGNullWindowID);
    if (rawWindows == nullptr) return nil;
    NSArray* windows = CFBridgingRelease(rawWindows);
    for (NSDictionary* info in windows) {
        if ([info[(id)kCGWindowNumber] unsignedIntValue] == windowId) return info;
    }
    return nil;
}

QString axErrorName(AXError error) {
    switch (error) {
    case kAXErrorSuccess: return QStringLiteral("success");
    case kAXErrorFailure: return QStringLiteral("failure");
    case kAXErrorIllegalArgument: return QStringLiteral("illegal-argument");
    case kAXErrorInvalidUIElement: return QStringLiteral("invalid-ui-element");
    case kAXErrorInvalidUIElementObserver: return QStringLiteral("invalid-ui-element-observer");
    case kAXErrorCannotComplete: return QStringLiteral("cannot-complete");
    case kAXErrorAttributeUnsupported: return QStringLiteral("attribute-unsupported");
    case kAXErrorActionUnsupported: return QStringLiteral("action-unsupported");
    case kAXErrorNotificationUnsupported: return QStringLiteral("notification-unsupported");
    case kAXErrorNotImplemented: return QStringLiteral("not-implemented");
    case kAXErrorNotificationAlreadyRegistered: return QStringLiteral("notification-already-registered");
    case kAXErrorNotificationNotRegistered: return QStringLiteral("notification-not-registered");
    case kAXErrorAPIDisabled: return QStringLiteral("api-disabled");
    case kAXErrorNoValue: return QStringLiteral("no-value");
    case kAXErrorParameterizedAttributeUnsupported:
        return QStringLiteral("parameterized-attribute-unsupported");
    case kAXErrorNotEnoughPrecision: return QStringLiteral("not-enough-precision");
    default: return QStringLiteral("unknown-%1").arg(static_cast<int>(error));
    }
}

}  // namespace

}  // namespace quizpane::external_window

using namespace quizpane::external_window;

@implementation QPMacReplicaController

- (instancetype)initWithOwner:(quizpane::external_window::MacWindowReplicaBackend*)owner {
    self = [super init];
    if (self) {
        _owner = owner;
        _captureQueue = dispatch_queue_create("org.quizpane.external-window.capture", DISPATCH_QUEUE_SERIAL);
    }
    return self;
}

- (void)notifyResult:(const quizpane::external_window::AttachResult&)result {
    if (_reported) return;
    _reported = YES;
    qInfo() << "[QuizPane][ExternalWindow] attach completed"
            << result.success << result.sessionId;
    auto* owner = _owner;
    if (owner == nullptr) return;
    QMetaObject::invokeMethod(owner, [owner, result] {
        owner->reportAttachFinished(result);
    }, Qt::QueuedConnection);
}

- (void)notifyFailure:(const QString&)error {
    qWarning().noquote() << "[QuizPane][ExternalWindow]" << error;
    [self notifyResult:failureResult(_request, error)];
}

- (void)startWithRequest:(const quizpane::external_window::AttachRequest&)request {
    const BOOL isTargetLookupRetry = _retryingTargetLookup;
    _retryingTargetLookup = NO;
    [self detach];
    _request = request;
    _reported = NO;
    const NSUInteger generation = ++_generation;
    if (!@available(macOS 13.0, *)) {
        [self notifyFailure:QStringLiteral("网页视频置顶需要 macOS 13 或更高版本")];
        return;
    }
    // 不把 CGPreflightScreenCaptureAccess 当作最终结论。macOS 的“屏幕录制与
    // 系统录音”新面板在 App 刚更新或被 ad-hoc 签名时，可能已经显示为已允许，
    // 但这个旧式预检仍短暂返回 false。真正的 ScreenCaptureKit 请求才是权限的
    // 权威结果；它不会在这里主动弹授权框，未授权时会在回调中返回具体错误。
    NSString* rendererError = nil;
    if (![self prepareRenderer:&rendererError]) {
        [self notifyFailure:QString::fromNSString(rendererError)];
        return;
    }

    __weak QPMacReplicaController* weakSelf = self;
    // Chromium 同步 document.title 到原生窗口标题是异步的。首次短等，然后若未
    // 找到目标按 200ms、400ms 指数退避再查两次；整个绑定过程仍在一秒内完成。
    if (!isTargetLookupRetry) _targetLookupAttempt = 0;
    void (^lookupTarget)(void);
    lookupTarget = ^{
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                               onScreenWindowsOnly:NO
                                                 completionHandler:^(SCShareableContent* content, NSError* error) {
        QPMacReplicaController* strongSelf = weakSelf;
        if (strongSelf == nil || strongSelf->_reported || generation != strongSelf->_generation) return;
        if (error != nil || content == nil) {
            [strongSelf notifyFailure:QString::fromNSString(error.localizedDescription ?: @"无法读取可捕获窗口")];
            return;
        }
        NSString* token = strongSelf->_request.bindingToken.toNSString();
        SCWindow* target = nil;
        SCWindow* closestBrowserWindow = nil;
        CGFloat closestSizeDelta = CGFLOAT_MAX;
        for (SCWindow* candidate in content.windows) {
            if (!supportedBrowserBundle(candidate.owningApplication.bundleIdentifier)) continue;
            if (candidate.title != nil && [candidate.title containsString:token]) {
                target = candidate;
                break;
            }
            // Chromium 有时不会把 document.title 及时同步到原生窗口标题。扩展同时
            // 传来了 popup 的真实尺寸；用它作为受限的兜底匹配，避免误选主浏览器窗。
            const CGFloat sizeDelta = std::abs(candidate.frame.size.width -
                                                strongSelf->_request.browserReportedBounds.width()) +
                                      std::abs(candidate.frame.size.height -
                                                strongSelf->_request.browserReportedBounds.height());
            if (sizeDelta < closestSizeDelta) {
                closestSizeDelta = sizeDelta;
                closestBrowserWindow = candidate;
            }
        }
        if (target == nil && closestBrowserWindow != nil && closestSizeDelta <= 96.0) {
            target = closestBrowserWindow;
        }
        if (target == nil) {
            if (strongSelf->_targetLookupAttempt < 2) {
                const NSUInteger retry = ++strongSelf->_targetLookupAttempt;
                const int delayMs = 150 * (1 << (retry - 1));
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                              static_cast<int64_t>(delayMs * NSEC_PER_MSEC)),
                               dispatch_get_main_queue(), ^{
                    QPMacReplicaController* retrySelf = weakSelf;
                    if (retrySelf != nil && !retrySelf->_reported &&
                        generation == retrySelf->_generation) {
                        retrySelf->_retryingTargetLookup = YES;
                        [retrySelf startWithRequest:retrySelf->_request];
                    }
                });
                return;
            }
            [strongSelf notifyResult:failureResult(
                strongSelf->_request,
                QStringLiteral("没有找到已绑定的 Chrome 或 Edge 课程小窗，请重新打开课程小窗"),
                AttachError::TargetNotFound)];
            return;
        }
        // SCShareableContent 的回调来自 replayd 的 XPC 工作线程；所有 AppKit
        // 窗口操作必须切回主线程，否则 NSPanel 会触发 Objective-C abort。
        dispatch_async(dispatch_get_main_queue(), ^{
        if (strongSelf->_reported || generation != strongSelf->_generation) return;
        strongSelf->_sourceWindowId = target.windowID;
        strongSelf->_sourceProcessId = target.owningApplication.processID;
        NSDictionary* sourceInfo = windowInfoForId(target.windowID);
        const bool sourceOnScreen = [sourceInfo[(id)kCGWindowIsOnscreen] boolValue];
        quizpane::diagnostic::event(QStringLiteral("mac-source-window"),
                          QStringLiteral("target-resolved"),
                          {{QStringLiteral("sessionId"), strongSelf->_request.sessionId},
                           {QStringLiteral("windowId"), static_cast<qulonglong>(target.windowID)},
                           {QStringLiteral("pid"), static_cast<qlonglong>(target.owningApplication.processID)},
                           {QStringLiteral("titleMatched"), target.title != nil &&
                            [target.title containsString:strongSelf->_request.bindingToken.toNSString()]},
                           {QStringLiteral("width"), target.frame.size.width},
                           {QStringLiteral("height"), target.frame.size.height},
                           {QStringLiteral("isOnScreen"), sourceOnScreen}});
        [strongSelf showPanelForBounds:strongSelf->_request.browserReportedBounds];
        SCContentFilter* filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:target];
        SCStreamConfiguration* config = [SCStreamConfiguration new];
        const int sourceWidth = std::max(1, static_cast<int>(target.frame.size.width));
        const int sourceHeight = std::max(1, static_cast<int>(target.frame.size.height));
        CGFloat backingScale = NSScreen.mainScreen.backingScaleFactor;
        for (NSScreen* screen in NSScreen.screens) {
            if (NSIntersectsRect(screen.frame, target.frame)) {
                backingScale = screen.backingScaleFactor;
                break;
            }
        }
        // SCWindow.frame 是逻辑点，SCStreamConfiguration 需要实际像素。之前把
        // 640pt 直接当 640px，Retina 屏上会白白损失一半分辨率。
        const double pixelWidth = sourceWidth * std::max<CGFloat>(1.0, backingScale);
        const double pixelHeight = sourceHeight * std::max<CGFloat>(1.0, backingScale);
        const double scale = std::min(1.0, std::min(kMaxCaptureWidth / pixelWidth,
                                                    kMaxCaptureHeight / pixelHeight));
        config.width = std::max(1, static_cast<int>(pixelWidth * scale));
        config.height = std::max(1, static_cast<int>(pixelHeight * scale));
        config.pixelFormat = kCVPixelFormatType_32BGRA;
        config.minimumFrameInterval = CMTimeMake(1, std::clamp(strongSelf->_request.preferredFps, 15, 60));
        config.queueDepth = 3;
        config.showsCursor = NO;
        config.capturesAudio = NO;
        strongSelf->_stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:strongSelf];
        @synchronized (strongSelf) {
            strongSelf->_frameSerial = 0;
            strongSelf->_awaitingInitialPresentation = YES;
            strongSelf->_presentationPending = NO;
        }
        NSError* streamError = nil;
        if (![strongSelf->_stream addStreamOutput:strongSelf
                                              type:SCStreamOutputTypeScreen
                                sampleHandlerQueue:strongSelf->_captureQueue
                                             error:&streamError]) {
            [strongSelf notifyFailure:QString::fromNSString(streamError.localizedDescription ?: @"无法添加视频帧输出")];
            return;
        }
        [strongSelf->_stream startCaptureWithCompletionHandler:^(NSError* startError) {
            if (generation != strongSelf->_generation) return;
            if (startError != nil) {
                [strongSelf notifyFailure:QString::fromNSString(startError.localizedDescription ?: @"无法启动网页小窗捕获")];
                return;
            }
            // startCapture 成功只代表流已启动，不代表镜像已经收到并显示视频帧。
            // 真正的成功回执在首帧完成 Metal 呈现后发出。
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 4 * NSEC_PER_SEC),
                           dispatch_get_main_queue(), ^{
                QPMacReplicaController* timeoutSelf = weakSelf;
                if (timeoutSelf != nil && !timeoutSelf->_reported &&
                    generation == timeoutSelf->_generation) {
                    [timeoutSelf notifyFailure:QStringLiteral("视频画面暂时没有准备好，请重试")];
                }
            });
        }];
        });
    }];
    };
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(100 * NSEC_PER_MSEC)),
                   dispatch_get_main_queue(), ^{ lookupTarget(); });
}

- (void)detach {
    ++_generation;
    if (_stream != nil) {
        [_stream stopCaptureWithCompletionHandler:^(__unused NSError* error) {}];
        _stream = nil;
    }
    if (_sourceWasAccessibilityParked && _sourceAxWindow != nullptr &&
        _sourceHasOriginalPosition) {
        AXValueRef restoreValue = AXValueCreate(static_cast<AXValueType>(kAXValueCGPointType),
                                                &_sourceOriginalPosition);
        const AXError restoreError = AXUIElementSetAttributeValue(
            _sourceAxWindow, kAXPositionAttribute, restoreValue);
        if (restoreValue != nullptr) CFRelease(restoreValue);
        quizpane::diagnostic::event(QStringLiteral("mac-source-window"),
                          QStringLiteral("ax-park-restore-on-detach"),
                          {{QStringLiteral("success"), restoreError == kAXErrorSuccess},
                           {QStringLiteral("error"), axErrorName(restoreError)}});
    }
    if (_sourceAxWindow != nullptr) {
        CFRelease(_sourceAxWindow);
        _sourceAxWindow = nullptr;
    }
    _sourceWindowId = kCGNullWindowID;
    _sourceProcessId = 0;
    _sourceHasOriginalPosition = NO;
    _sourceWasAccessibilityParked = NO;
    if (_panel != nil) {
        quizpane::diagnostic::event(QStringLiteral("mac-mirror"), QStringLiteral("panel-detach"),
                          {{QStringLiteral("sessionId"), _request.sessionId},
                           {QStringLiteral("visible"), _panel.isVisible},
                           {QStringLiteral("left"), _panel.frame.origin.x},
                           {QStringLiteral("top"), _panel.frame.origin.y},
                           {QStringLiteral("width"), _panel.frame.size.width},
                           {QStringLiteral("height"), _panel.frame.size.height},
                           {QStringLiteral("level"), static_cast<qlonglong>(_panel.level)}});
        [_panel orderOut:nil];
        _panel = nil;
        _metalView = nil;
    }
    @synchronized (self) {
        _latestTexture = nil;
        if (_latestCvTexture != nil) {
            CFRelease(_latestCvTexture);
            _latestCvTexture = nil;
        }
    }
    if (_textureCache != nil) {
        CFRelease(_textureCache);
        _textureCache = nil;
    }
    @synchronized (self) {
        _awaitingInitialPresentation = NO;
        _awaitingRestorePresentation = NO;
        _presentationPending = NO;
        ++_restoreEpoch;
    }
}

- (void)beginAccessibilitySourceParkingProbe {
#ifndef QUIZPANE_DIAGNOSTIC_LOGGING
    // 正式包不主动索取辅助功能授权。这条实验仅在用户安装的诊断包中开启。
    return;
#else
    if (_sourceWindowId == kCGNullWindowID || _sourceProcessId <= 0 ||
        _sourceAxWindow != nullptr) {
        return;
    }

    const NSDictionary* promptOptions = @{
        (__bridge id)kAXTrustedCheckOptionPrompt : @YES,
    };
    const bool trusted = AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)promptOptions);
    quizpane::diagnostic::event(QStringLiteral("mac-source-window"),
                      QStringLiteral("ax-authorization"),
                      {{QStringLiteral("sessionId"), _request.sessionId},
                       {QStringLiteral("trusted"), trusted},
                       {QStringLiteral("promptRequested"), true}});
    if (!trusted) {
        quizpane::diagnostic::event(QStringLiteral("mac-source-window"),
                          QStringLiteral("ax-park-unavailable"),
                          {{QStringLiteral("reason"), QStringLiteral("accessibility-not-trusted")}});
        return;
    }

    AXUIElementRef app = AXUIElementCreateApplication(_sourceProcessId);
    CFTypeRef rawWindows = nullptr;
    const AXError listError = AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, &rawWindows);
    if (app != nullptr) CFRelease(app);
    if (listError != kAXErrorSuccess || rawWindows == nullptr ||
        CFGetTypeID(rawWindows) != CFArrayGetTypeID()) {
        if (rawWindows != nullptr) CFRelease(rawWindows);
        quizpane::diagnostic::event(QStringLiteral("mac-source-window"),
                          QStringLiteral("ax-park-unavailable"),
                          {{QStringLiteral("reason"), QStringLiteral("windows-unreadable")},
                           {QStringLiteral("error"), axErrorName(listError)}});
        return;
    }

    AXUIElementRef matched = nullptr;
    NSString* token = _request.bindingToken.toNSString();
    for (id item in (__bridge NSArray*)rawWindows) {
        AXUIElementRef window = (__bridge AXUIElementRef)item;
        CFTypeRef rawTitle = nullptr;
        const AXError titleError = AXUIElementCopyAttributeValue(window, kAXTitleAttribute, &rawTitle);
        const NSString* title = titleError == kAXErrorSuccess && rawTitle != nullptr &&
            CFGetTypeID(rawTitle) == CFStringGetTypeID() ? (__bridge NSString*)rawTitle : nil;
        const BOOL isMatch = title != nil && [title containsString:token];
        if (rawTitle != nullptr) CFRelease(rawTitle);
        if (isMatch) {
            matched = (AXUIElementRef)CFRetain(window);
            break;
        }
    }
    CFRelease(rawWindows);
    if (matched == nullptr) {
        quizpane::diagnostic::event(QStringLiteral("mac-source-window"),
                          QStringLiteral("ax-park-unavailable"),
                          {{QStringLiteral("reason"), QStringLiteral("bound-window-not-found")},
                           {QStringLiteral("pid"), static_cast<qlonglong>(_sourceProcessId)}});
        return;
    }

    Boolean settable = false;
    const AXError settableError = AXUIElementIsAttributeSettable(
        matched, kAXPositionAttribute, &settable);
    CFTypeRef rawPosition = nullptr;
    const AXError readError = AXUIElementCopyAttributeValue(
        matched, kAXPositionAttribute, &rawPosition);
    CGPoint originalPosition = CGPointZero;
    const BOOL hasPosition = readError == kAXErrorSuccess && rawPosition != nullptr &&
        AXValueGetType((AXValueRef)rawPosition) == kAXValueCGPointType &&
        AXValueGetValue((AXValueRef)rawPosition,
                        static_cast<AXValueType>(kAXValueCGPointType), &originalPosition);
    if (rawPosition != nullptr) CFRelease(rawPosition);
    if (settableError != kAXErrorSuccess || !settable || !hasPosition) {
        CFRelease(matched);
        quizpane::diagnostic::event(QStringLiteral("mac-source-window"),
                          QStringLiteral("ax-park-unavailable"),
                          {{QStringLiteral("reason"), QStringLiteral("position-not-writable")},
                           {QStringLiteral("settable"), static_cast<bool>(settable)},
                           {QStringLiteral("settableError"), axErrorName(settableError)},
                           {QStringLiteral("readError"), axErrorName(readError)}});
        return;
    }

    CGDirectDisplayID display = CGMainDisplayID();
    CGDirectDisplayID containingDisplay = kCGNullDirectDisplay;
    const CGPoint sourceCenter = CGPointMake(originalPosition.x + 1.0, originalPosition.y + 1.0);
    if (CGGetDisplaysWithPoint(sourceCenter, 1, &containingDisplay, nullptr) == kCGErrorSuccess &&
        containingDisplay != kCGNullDirectDisplay) {
        display = containingDisplay;
    }
    const CGRect displayBounds = CGDisplayBounds(display);
    const CGPoint requestedPosition = CGPointMake(
        CGRectGetMaxX(displayBounds) - kAccessibilityParkExposeWidth,
        CGRectGetMaxY(displayBounds) - kAccessibilityParkExposeHeight);
    AXValueRef requestedValue = AXValueCreate(static_cast<AXValueType>(kAXValueCGPointType),
                                              &requestedPosition);
    const AXError writeError = AXUIElementSetAttributeValue(
        matched, kAXPositionAttribute, requestedValue);
    if (requestedValue != nullptr) CFRelease(requestedValue);

    CFTypeRef rawActualPosition = nullptr;
    const AXError actualReadError = AXUIElementCopyAttributeValue(
        matched, kAXPositionAttribute, &rawActualPosition);
    CGPoint actualPosition = CGPointZero;
    const BOOL hasActualPosition = actualReadError == kAXErrorSuccess && rawActualPosition != nullptr &&
        AXValueGetType((AXValueRef)rawActualPosition) == kAXValueCGPointType &&
        AXValueGetValue((AXValueRef)rawActualPosition,
                        static_cast<AXValueType>(kAXValueCGPointType), &actualPosition);
    if (rawActualPosition != nullptr) CFRelease(rawActualPosition);

    quizpane::diagnostic::event(QStringLiteral("mac-source-window"),
                      QStringLiteral("ax-park-result"),
                      {{QStringLiteral("success"), writeError == kAXErrorSuccess && hasActualPosition},
                       {QStringLiteral("writeError"), axErrorName(writeError)},
                       {QStringLiteral("readError"), axErrorName(actualReadError)},
                       {QStringLiteral("requestedLeft"), requestedPosition.x},
                       {QStringLiteral("requestedTop"), requestedPosition.y},
                       {QStringLiteral("actualLeft"), actualPosition.x},
                       {QStringLiteral("actualTop"), actualPosition.y},
                       {QStringLiteral("originalLeft"), originalPosition.x},
                       {QStringLiteral("originalTop"), originalPosition.y},
                       {QStringLiteral("displayWidth"), displayBounds.size.width},
                       {QStringLiteral("displayHeight"), displayBounds.size.height}});
    if (writeError != kAXErrorSuccess || !hasActualPosition) {
        CFRelease(matched);
        return;
    }

    _sourceAxWindow = matched;
    _sourceOriginalPosition = originalPosition;
    _sourceHasOriginalPosition = YES;
    _sourceWasAccessibilityParked = YES;
    @synchronized (self) {
        _sourceParkBaselineFrame = _frameSerial;
    }
    const NSUInteger generation = _generation;
    __weak QPMacReplicaController* weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
        QPMacReplicaController* strongSelf = weakSelf;
        if (strongSelf == nil || generation != strongSelf->_generation ||
            !strongSelf->_sourceWasAccessibilityParked) return;
        NSUInteger currentFrame = 0;
        @synchronized (strongSelf) { currentFrame = strongSelf->_frameSerial; }
        const NSUInteger freshFrames = currentFrame - strongSelf->_sourceParkBaselineFrame;
        NSDictionary* sourceInfo = windowInfoForId(strongSelf->_sourceWindowId);
        const bool onScreen = [sourceInfo[(id)kCGWindowIsOnscreen] boolValue];
        const bool healthy = onScreen && freshFrames >= kAccessibilityParkMinimumFreshFrames;
        quizpane::diagnostic::event(QStringLiteral("mac-source-window"),
                          QStringLiteral("ax-park-health"),
                          {{QStringLiteral("healthy"), healthy},
                           {QStringLiteral("isOnScreen"), onScreen},
                           {QStringLiteral("freshFrames"), static_cast<qulonglong>(freshFrames)},
                           {QStringLiteral("baselineFrame"),
                            static_cast<qulonglong>(strongSelf->_sourceParkBaselineFrame)},
                           {QStringLiteral("currentFrame"), static_cast<qulonglong>(currentFrame)}});
        if (healthy || strongSelf->_sourceAxWindow == nullptr ||
            !strongSelf->_sourceHasOriginalPosition) return;
        AXValueRef restoreValue = AXValueCreate(static_cast<AXValueType>(kAXValueCGPointType),
                                                &strongSelf->_sourceOriginalPosition);
        const AXError restoreError = AXUIElementSetAttributeValue(
            strongSelf->_sourceAxWindow, kAXPositionAttribute, restoreValue);
        if (restoreValue != nullptr) CFRelease(restoreValue);
        strongSelf->_sourceWasAccessibilityParked = NO;
        quizpane::diagnostic::event(QStringLiteral("mac-source-window"),
                          QStringLiteral("ax-park-rollback"),
                          {{QStringLiteral("success"), restoreError == kAXErrorSuccess},
                           {QStringLiteral("error"), axErrorName(restoreError)},
                           {QStringLiteral("reason"), QStringLiteral("source-frame-stalled")}});
    });
#endif
}

- (void)activateMirrorPanel {
    if (_panel == nil) return;
    // 等价于用户从 Edge 点击回 QuizPane：浏览器课程窗仍保持 normal，
    // 但 Edge 失去前台应用状态。不能用最小化源窗代替应用激活权切换，
    // 否则 Chromium 会停止持续合成，ScreenCaptureKit 镜像便停在最后一帧。
    [NSApp activateIgnoringOtherApps:YES];
    [_panel makeKeyAndOrderFront:nil];
    [_panel makeFirstResponder:_metalView];
    quizpane::diagnostic::event(QStringLiteral("mac-mirror"), QStringLiteral("panel-activated"),
                      {{QStringLiteral("sessionId"), _request.sessionId},
                       {QStringLiteral("visible"), _panel.isVisible},
                       {QStringLiteral("key"), _panel.isKeyWindow},
                       {QStringLiteral("left"), _panel.frame.origin.x},
                       {QStringLiteral("top"), _panel.frame.origin.y},
                       {QStringLiteral("width"), _panel.frame.size.width},
                       {QStringLiteral("height"), _panel.frame.size.height},
                       {QStringLiteral("level"), static_cast<qlonglong>(_panel.level)}});
}

- (void)setVisible:(BOOL)visible {
    if (_panel == nil) return;
    quizpane::diagnostic::event(QStringLiteral("mac-mirror"), QStringLiteral("panel-visibility-request"),
                      {{QStringLiteral("visible"), static_cast<bool>(visible)},
                       {QStringLiteral("panelVisible"), _panel.isVisible},
                       {QStringLiteral("frameSerial"), static_cast<qulonglong>(_frameSerial)}});
    if (visible) {
        // orderOut 后仅 orderFront 不会恢复 MTKView 的首响应者状态，鼠标事件会
        // 落空。恢复老板键时重新让镜像面板成为可交互窗口。
        [self activateMirrorPanel];
    } else {
        @synchronized (self) {
            _awaitingRestorePresentation = NO;
            _presentationPending = NO;
            ++_restoreEpoch;
        }
        [_panel orderOut:nil];
    }
}

- (void)prepareForRestore {
    if (_panel == nil || _stream == nil) {
        quizpane::diagnostic::event(QStringLiteral("boss-restore"), QStringLiteral("prepare-skipped"),
                          {{QStringLiteral("hasPanel"), _panel != nil},
                           {QStringLiteral("hasStream"), _stream != nil}});
        return;
    }
    NSUInteger epoch;
    @synchronized (self) {
        _awaitingRestorePresentation = YES;
        _presentationPending = NO;
        _restoreBaselineFrame = _frameSerial;
        epoch = ++_restoreEpoch;
        qInfo() << "[QuizPane][BossRestore] awaiting frame after"
                << _restoreBaselineFrame << "epoch" << epoch;
        quizpane::diagnostic::event(QStringLiteral("boss-restore"), QStringLiteral("prepare"),
                          {{QStringLiteral("baselineFrame"),
                            static_cast<qulonglong>(_restoreBaselineFrame)},
                           {QStringLiteral("epoch"), static_cast<qulonglong>(epoch)},
                           {QStringLiteral("hasCachedTexture"), _latestTexture != nil},
                           {QStringLiteral("panelVisible"), _panel.isVisible}});
    }
    [_panel orderOut:nil];

    // 静止画面可能不会持续产生完整帧。超时后使用已有的最后一帧完成呈现，
    // 但仍然经过 Metal 提交确认，避免重新退回固定延迟后直接最小化源窗。
    __weak QPMacReplicaController* weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
        QPMacReplicaController* strongSelf = weakSelf;
        if (strongSelf == nil) return;
        BOOL shouldFallback = NO;
        @synchronized (strongSelf) {
            shouldFallback = strongSelf->_awaitingRestorePresentation &&
                strongSelf->_restoreEpoch == epoch && strongSelf->_latestTexture != nil;
        }
        if (shouldFallback) {
            qInfo() << "[QuizPane][BossRestore] no fresh frame; presenting cached frame"
                    << "epoch" << epoch;
            quizpane::diagnostic::event(QStringLiteral("boss-restore"),
                              QStringLiteral("cached-frame-fallback"),
                              {{QStringLiteral("epoch"), static_cast<qulonglong>(epoch)},
                               {QStringLiteral("frameSerial"),
                                static_cast<qulonglong>(strongSelf->_frameSerial)},
                               {QStringLiteral("presentationPending"),
                                strongSelf->_presentationPending}});
            [strongSelf activateMirrorPanel];
            [strongSelf->_metalView draw];
        } else {
            quizpane::diagnostic::event(QStringLiteral("boss-restore"),
                              QStringLiteral("cached-frame-fallback-skipped"),
                              {{QStringLiteral("epoch"), static_cast<qulonglong>(epoch)},
                               {QStringLiteral("awaiting"),
                                strongSelf->_awaitingRestorePresentation},
                               {QStringLiteral("hasCachedTexture"),
                                strongSelf->_latestTexture != nil},
                               {QStringLiteral("presentationPending"),
                                strongSelf->_presentationPending}});
        }
    });
}

- (void)invalidateOwner {
    _owner = nullptr;
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    if (stream == _stream && error != nil) {
        [self notifyFailure:QString::fromNSString(error.localizedDescription ?: @"网页小窗捕获已停止")];
    }
}

@end
