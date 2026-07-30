#pragma once

#include "mac_window_replica_backend.hpp"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

// macOS 后端的核心实现：不直接操作源浏览器窗口本身，而是用
// ScreenCaptureKit（SCStream）持续截取源窗口画面，塞进 Metal 纹理，
// 渲染到一个自建的 NSPanel（_panel/_metalView）里冒充"置顶小窗"。
// 之所以不像 Windows 那样直接把源窗口置顶，是因为 macOS 没有等价 API
// 能让另一个 App 的窗口脱离其原有层级、稳定悬浮在所有空间之上。
//
// 各分类实现分散在不同 .mm 文件里（同一个类用 Objective-C category
// 拆开，减少单文件体积）：
//   mac_window_replica_capture.mm  — 生命周期、SCStream 建立/销毁、AX 停靠
//   mac_window_replica_renderer.mm — Metal 管线搭建、每帧渲染
//   mac_window_replica_input.mm    — 镜像面板内的鼠标事件转发回 Qt
@interface QPMacReplicaController : NSObject <SCStreamDelegate> {
@public
    quizpane::external_window::MacWindowReplicaBackend* _owner;
    SCStream* _stream;
    NSPanel* _panel;
    MTKView* _metalView;
    id<MTLDevice> _device;
    id<MTLCommandQueue> _commandQueue;
    id<MTLRenderPipelineState> _pipeline;
    id<MTLSamplerState> _sampler;
    CVMetalTextureCacheRef _textureCache;
    CVMetalTextureRef _latestCvTexture;
    id<MTLTexture> _latestTexture;
    dispatch_queue_t _captureQueue;
    quizpane::external_window::AttachRequest _request;
    BOOL _reported;
    NSUInteger _generation;
    NSUInteger _targetLookupAttempt;
    BOOL _retryingTargetLookup;
    BOOL _awaitingInitialPresentation;
    BOOL _awaitingRestorePresentation;
    BOOL _presentationPending;
    NSUInteger _frameSerial;
    NSUInteger _restoreBaselineFrame;
    NSUInteger _restoreEpoch;
    CGWindowID _sourceWindowId;
    pid_t _sourceProcessId;
    AXUIElementRef _sourceAxWindow;
    CGPoint _sourceOriginalPosition;
    BOOL _sourceHasOriginalPosition;
    BOOL _sourceWasAccessibilityParked;
    NSUInteger _sourceParkBaselineFrame;
    BOOL _seeking;
    NSPoint _pointerDown;
    CFTimeInterval _lastSeekEmissionTime;
}
- (instancetype)initWithOwner:(quizpane::external_window::MacWindowReplicaBackend*)owner;
- (void)startWithRequest:(const quizpane::external_window::AttachRequest&)request;
- (void)detach;
- (void)setVisible:(BOOL)visible;
- (void)prepareForRestore;
- (void)activateMirrorPanel;
// 正式功能，在所有构建配置下无条件执行；详见 mac_window_replica_capture.mm
// 中的实现注释。
- (void)beginAccessibilitySourceParkingProbe;
- (void)invalidateOwner;
- (void)notifyResult:(const quizpane::external_window::AttachResult&)result;
@end

@interface QPMacReplicaController (MetalRendering) <SCStreamOutput, MTKViewDelegate>
- (BOOL)prepareRenderer:(NSString**)errorMessage;
- (void)showPanelForBounds:(const QRect&)bounds;
@end

@interface QPMacReplicaController (InputForwarding)
- (void)handleMirrorPointer:(NSPoint)point phase:(NSInteger)phase viewSize:(NSSize)viewSize;
@end

@interface QPMirrorView : MTKView
- (instancetype)initWithFrame:(NSRect)frame
                       device:(id<MTLDevice>)device
                   controller:(QPMacReplicaController*)controller;
@end
