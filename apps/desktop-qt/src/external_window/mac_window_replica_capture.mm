#include "mac_window_replica_controller.hpp"

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
    if (_panel != nil) {
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

- (void)activateMirrorPanel {
    if (_panel == nil) return;
    // 等价于用户从 Edge 点击回 QuizPane：浏览器课程窗仍保持 normal，
    // 但 Edge 失去前台应用状态。不能用最小化源窗代替应用激活权切换，
    // 否则 Chromium 会停止持续合成，ScreenCaptureKit 镜像便停在最后一帧。
    [NSApp activateIgnoringOtherApps:YES];
    [_panel makeKeyAndOrderFront:nil];
    [_panel makeFirstResponder:_metalView];
}

- (void)setVisible:(BOOL)visible {
    if (_panel == nil) return;
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
    if (_panel == nil || _stream == nil) return;
    NSUInteger epoch;
    @synchronized (self) {
        _awaitingRestorePresentation = YES;
        _presentationPending = NO;
        _restoreBaselineFrame = _frameSerial;
        epoch = ++_restoreEpoch;
        qInfo() << "[QuizPane][BossRestore] awaiting frame after"
                << _restoreBaselineFrame << "epoch" << epoch;
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
            [strongSelf activateMirrorPanel];
            [strongSelf->_metalView draw];
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
