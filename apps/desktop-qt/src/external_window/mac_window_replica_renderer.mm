#include "mac_window_replica_controller.hpp"
#include "quizpane/diagnostic_logger.hpp"

#import <QuartzCore/QuartzCore.h>

#include <QDebug>

#include <algorithm>

namespace {

constexpr int kMinWindowWidth = 320;
constexpr int kMinWindowHeight = 220;

quizpane::external_window::AttachResult successResult(
    const quizpane::external_window::AttachRequest& request) {
    using namespace quizpane::external_window;
    AttachResult result;
    result.sessionId = request.sessionId;
    result.success = true;
    result.backend = BackendType::MacCapturedReplica;
    result.capabilities = {false, true, true, true, true};
    return result;
}

}  // namespace

@implementation QPMacReplicaController (MetalRendering)

- (BOOL)prepareRenderer:(NSString**)errorMessage {
    _device = MTLCreateSystemDefaultDevice();
    if (_device == nil) {
        *errorMessage = @"此 Mac 没有可用的 Metal 渲染设备";
        return NO;
    }
    _commandQueue = [_device newCommandQueue];
    const CVReturn cacheStatus = CVMetalTextureCacheCreate(
        kCFAllocatorDefault, nullptr, _device, nullptr, &_textureCache);
    if (cacheStatus != kCVReturnSuccess) {
        *errorMessage = @"无法创建视频帧纹理缓存";
        return NO;
    }
    static NSString* const shaderSource = @R"metal(
        #include <metal_stdlib>
        using namespace metal;
        struct Raster { float4 position [[position]]; float2 uv; };
        vertex Raster vertexMain(uint id [[vertex_id]]) {
            float2 points[6] = {
                float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0),
                float2(-1.0, 1.0), float2(1.0, -1.0), float2(1.0, 1.0)
            };
            Raster out;
            out.position = float4(points[id], 0.0, 1.0);
            out.uv = float2((points[id].x + 1.0) * 0.5, (1.0 - points[id].y) * 0.5);
            return out;
        }
        fragment float4 fragmentMain(Raster in [[stage_in]],
                                     texture2d<float> frame [[texture(0)]],
                                     sampler textureSampler [[sampler(0)]]) {
            return frame.sample(textureSampler, in.uv);
        }
    )metal";
    NSError* libraryError = nil;
    id<MTLLibrary> library = [_device newLibraryWithSource:shaderSource
                                                   options:nil
                                                     error:&libraryError];
    if (library == nil) {
        *errorMessage = libraryError.localizedDescription ?: @"无法编译视频渲染着色器";
        return NO;
    }
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"vertexMain"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"fragmentMain"];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    NSError* pipelineError = nil;
    _pipeline = [_device newRenderPipelineStateWithDescriptor:descriptor error:&pipelineError];
    if (_pipeline == nil) {
        *errorMessage = pipelineError.localizedDescription ?: @"无法创建视频渲染管线";
        return NO;
    }
    MTLSamplerDescriptor* samplerDescriptor = [MTLSamplerDescriptor new];
    samplerDescriptor.minFilter = MTLSamplerMinMagFilterLinear;
    samplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
    samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    _sampler = [_device newSamplerStateWithDescriptor:samplerDescriptor];
    return YES;
}

- (void)showPanelForBounds:(const QRect&)bounds {
    const int width = std::max(kMinWindowWidth, bounds.width());
    const int height = std::max(kMinWindowHeight, bounds.height());
    _panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(96, 96, width, height)
        styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                   NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable)
        backing:NSBackingStoreBuffered
        defer:NO];
    _panel.title = @"课程视频小窗";
    _panel.level = NSFloatingWindowLevel;
    _panel.hidesOnDeactivate = NO;
    _panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
        NSWindowCollectionBehaviorFullScreenAuxiliary;
    QPMirrorView* mirrorView = [[QPMirrorView alloc]
        initWithFrame:_panel.contentView.bounds device:_device controller:self];
    _metalView = mirrorView;
    _metalView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _metalView.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    _metalView.framebufferOnly = YES;
    _metalView.enableSetNeedsDisplay = NO;
    _metalView.paused = NO;
    _metalView.preferredFramesPerSecond = std::clamp(_request.preferredFps, 15, 60);
    _metalView.delegate = self;
    [_panel setContentView:_metalView];
    [_panel orderOut:nil];
    quizpane::diagnostic::event(QStringLiteral("mac-mirror"), QStringLiteral("panel-created"),
                      {{QStringLiteral("sessionId"), _request.sessionId},
                       {QStringLiteral("left"), _panel.frame.origin.x},
                       {QStringLiteral("top"), _panel.frame.origin.y},
                       {QStringLiteral("width"), _panel.frame.size.width},
                       {QStringLiteral("height"), _panel.frame.size.height},
                       {QStringLiteral("level"), static_cast<qlonglong>(_panel.level)},
                       {QStringLiteral("visible"), _panel.isVisible}});
}

- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
    ofType:(SCStreamOutputType)type {
    if (stream != _stream || type != SCStreamOutputTypeScreen
        || !CMSampleBufferIsValid(sampleBuffer)) return;
    CVPixelBufferRef pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (pixelBuffer == nil || _textureCache == nil) return;
    CVMetalTextureRef textureRef = nil;
    const CVReturn status = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault, _textureCache, pixelBuffer, nullptr,
        MTLPixelFormatBGRA8Unorm, CVPixelBufferGetWidth(pixelBuffer),
        CVPixelBufferGetHeight(pixelBuffer), 0, &textureRef);
    if (status != kCVReturnSuccess || textureRef == nil) return;
    id<MTLTexture> texture = CVMetalTextureGetTexture(textureRef);
    if (texture == nil) {
        CFRelease(textureRef);
        return;
    }
    @synchronized (self) {
        if (_latestCvTexture != nil) CFRelease(_latestCvTexture);
        _latestCvTexture = textureRef;
        _latestTexture = texture;
        ++_frameSerial;
    }
    BOOL shouldPresent = NO;
    NSUInteger presentingFrame = 0;
    @synchronized (self) {
        shouldPresent = _awaitingInitialPresentation
            || (_awaitingRestorePresentation && _frameSerial > _restoreBaselineFrame);
        presentingFrame = _frameSerial;
    }
    if (!shouldPresent) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        BOOL stillNeeded = NO;
        @synchronized (self) {
            stillNeeded = self->_awaitingInitialPresentation
                || (self->_awaitingRestorePresentation
                    && self->_frameSerial > self->_restoreBaselineFrame);
        }
        if (!stillNeeded || self->_panel == nil || self->_metalView == nil) return;
        qInfo() << "[QuizPane][BossRestore] presenting frame" << presentingFrame;
        [self activateMirrorPanel];
        [self->_metalView draw];
    });
}

- (void)drawInMTKView:(MTKView*)view {
    id<MTLTexture> texture = nil;
    BOOL confirmingInitial = NO;
    BOOL confirmingRestore = NO;
    NSUInteger restoreEpoch = 0;
    @synchronized (self) {
        texture = _latestTexture;
        if (!_presentationPending && texture != nil) {
            confirmingInitial = _awaitingInitialPresentation;
            confirmingRestore = !confirmingInitial && _awaitingRestorePresentation;
            if (confirmingInitial || confirmingRestore) {
                _presentationPending = YES;
                restoreEpoch = _restoreEpoch;
            }
        }
    }
    if (confirmingInitial || confirmingRestore) {
        // MTKView 在 Panel 已 orderOut 时仍可能用缓存纹理完成一次绘制。此前这条
        // 路径会把恢复握手标为完成，却从未重新 orderFront，导致老板键恢复后镜像
        // 窗口继续隐藏。开始确认呈现前统一把 Panel 前置，缓存帧和新帧行为一致。
        const auto presentPanel = ^{
            [self activateMirrorPanel];
            quizpane::diagnostic::event(QStringLiteral("mac-mirror"),
                                        QStringLiteral("presentation-panel-front"),
                                        {{QStringLiteral("initial"), confirmingInitial},
                                         {QStringLiteral("restore"), confirmingRestore},
                                         {QStringLiteral("restoreEpoch"),
                                          static_cast<qulonglong>(restoreEpoch)},
                                         {QStringLiteral("panelVisible"), _panel.isVisible},
                                         {QStringLiteral("panelLevel"),
                                          static_cast<qlonglong>(_panel.level)}});
        };
        if ([NSThread isMainThread]) presentPanel();
        else dispatch_async(dispatch_get_main_queue(), presentPanel);
    }
    MTLRenderPassDescriptor* pass = view.currentRenderPassDescriptor;
    id<CAMetalDrawable> drawable = view.currentDrawable;
    if (texture == nil || pass == nil || drawable == nil) {
        if (confirmingInitial || confirmingRestore) {
            BOOL clearedPending = NO;
            @synchronized (self) {
                if (_presentationPending && _restoreEpoch == restoreEpoch) {
                    _presentationPending = NO;
                    clearedPending = YES;
                }
            }
            quizpane::diagnostic::event(QStringLiteral("mac-mirror"),
                              QStringLiteral("presentation-unavailable"),
                              {{QStringLiteral("initial"), confirmingInitial},
                               {QStringLiteral("restore"), confirmingRestore},
                               {QStringLiteral("hasTexture"), texture != nil},
                               {QStringLiteral("hasRenderPass"), pass != nil},
                               {QStringLiteral("hasDrawable"), drawable != nil},
                               {QStringLiteral("panelVisible"), _panel.isVisible},
                               {QStringLiteral("presentationPending"), _presentationPending},
                               {QStringLiteral("clearedPending"), clearedPending},
                               {QStringLiteral("restoreEpoch"),
                                static_cast<qulonglong>(restoreEpoch)}});
        }
        return;
    }
    id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:_pipeline];
    [encoder setFragmentTexture:texture atIndex:0];
    [encoder setFragmentSamplerState:_sampler atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    [encoder endEncoding];
    [commandBuffer presentDrawable:drawable];
    if (confirmingInitial || confirmingRestore) {
        __weak QPMacReplicaController* weakSelf = self;
        [commandBuffer addCompletedHandler:^(__unused id<MTLCommandBuffer> completed) {
            dispatch_async(dispatch_get_main_queue(), ^{
                QPMacReplicaController* strongSelf = weakSelf;
                if (strongSelf == nil) return;
                BOOL finishInitial = NO;
                BOOL finishRestore = NO;
                @synchronized (strongSelf) {
                    finishInitial = confirmingInitial
                        && strongSelf->_awaitingInitialPresentation;
                    finishRestore = confirmingRestore
                        && strongSelf->_awaitingRestorePresentation
                        && strongSelf->_restoreEpoch == restoreEpoch;
                    if (finishInitial) strongSelf->_awaitingInitialPresentation = NO;
                    if (finishRestore) strongSelf->_awaitingRestorePresentation = NO;
                    strongSelf->_presentationPending = NO;
                }
                if (finishInitial) {
                    // 只有镜像确实完成首帧呈现后才进行 AX 停靠探针；若辅助功能
                    // 拒绝、Chrome 拒绝或帧流变慢，探针都会自行回滚而不影响 attach。
                    [strongSelf beginAccessibilitySourceParkingProbe];
                    [strongSelf notifyResult:successResult(strongSelf->_request)];
                } else if (finishRestore) {
                    qInfo() << "[QuizPane][BossRestore] Metal presentation completed"
                            << "epoch" << restoreEpoch;
                    quizpane::diagnostic::event(QStringLiteral("boss-restore"),
                                      QStringLiteral("metal-presentation-completed"),
                                      {{QStringLiteral("epoch"),
                                        static_cast<qulonglong>(restoreEpoch)},
                                       {QStringLiteral("frameSerial"),
                                        static_cast<qulonglong>(strongSelf->_frameSerial)}});
                    auto* owner = strongSelf->_owner;
                    if (owner != nullptr) owner->reportRestoreFrameReady();
                }
            });
        }];
    }
    [commandBuffer commit];
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
    (void)view;
    (void)size;
}

@end
