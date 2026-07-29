#pragma once

#include "mac_window_replica_backend.hpp"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

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
