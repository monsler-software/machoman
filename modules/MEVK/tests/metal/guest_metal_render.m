#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreVideo/CoreVideo.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <unistd.h>

static void check(int yes, const char *message) {
  if (!yes) {
    fprintf(stderr, "MEVK render guest FAIL: %s\n", message);
    abort();
  }
}
static unsigned scheduled_count, completed_count, failed_count;
static void handlers(id<MTLCommandBuffer> command) {
  // Capture a stack value, return, and only then commit: handlers must be
  // copied.
  int token = 73;
  [command addScheduledHandler:^(id<MTLCommandBuffer> cb) {
    check(token == 73 && cb.status >= MTLCommandBufferStatusScheduled,
          "copied scheduled block");
    [cb waitUntilScheduled];
    ++scheduled_count;
  }];
  [command addCompletedHandler:^(id<MTLCommandBuffer> cb) {
    check(token == 73 && cb.status == MTLCommandBufferStatusCompleted &&
              !cb.error,
          "copied completion block");
    [cb waitUntilCompleted];
    ++completed_count; // reentrant, once only
  }];
}
static void finish(id<MTLCommandBuffer> command) {
  [command commit];
  [command waitUntilCompleted];
  if (command.error)
    fprintf(stderr, "MEVK: %s\n",
            command.error.localizedDescription.UTF8String);
  check(command.status == MTLCommandBufferStatusCompleted && !command.error,
        "GPU command completed");
}
static MTLRenderPassDescriptor *pass(id<MTLTexture> texture) {
  MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
  p.colorAttachments[0].texture = texture;
  p.colorAttachments[0].loadAction = MTLLoadActionClear;
  p.colorAttachments[0].storeAction = MTLStoreActionStore;
  p.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1);
  MTLClearColor c = p.colorAttachments[0].clearColor;
  check(c.red == 0 && c.green == 0 && c.blue == 1 && c.alpha == 1,
        "clearColor four-double ABI");
  return p;
}
static void pixels(id<MTLTexture> texture, unsigned char *output) {
  [texture getBytes:output
        bytesPerRow:64 * 4
         fromRegion:MTLRegionMake2D(0, 0, 64, 64)
        mipmapLevel:0];
}
static void rectangle(const unsigned char *image) {
  for (unsigned y = 0; y < 64; ++y)
    for (unsigned x = 0; x < 64; ++x) {
      const unsigned char *p = image + (y * 64 + x) * 4;
      int inside = x >= 10 && x < 26 && y >= 10 && y < 26;
      if (abs((int)p[0] - (inside ? 128 : 255)) > 1 || p[1] != 0 ||
          abs((int)p[2] - (inside ? 128 : 0)) > 1 || p[3] != 255) {
        fprintf(stderr, "pixel %u,%u: BGRA %u %u %u %u (inside=%d)\n", x, y,
                p[0], p[1], p[2], p[3], inside);
        check(
            0,
            "guest AIR render, indexed arguments, scissor and blending pixels");
      }
    }
}

@interface MEVKTestView : NSView
@end
@implementation MEVKTestView
- (CALayer *)makeBackingLayer {
  return [CAMetalLayer layer];
}
@end

static CVReturn display_tick(CVDisplayLinkRef link, const CVTimeStamp *now,
                            const CVTimeStamp *output, CVOptionFlags in,
                            CVOptionFlags *out, void *context) {
  (void)link; (void)in; (void)out;
  check((now->flags & (kCVTimeStampVideoHostTimeValid | kCVTimeStampVideoRefreshPeriodValid)) ==
        (kCVTimeStampVideoHostTimeValid | kCVTimeStampVideoRefreshPeriodValid), "CVTimeStamp layout and flags");
  check(now->videoTimeScale > 0 && now->videoRefreshPeriod == 1000 &&
        output->videoTime == now->videoTime + 1000 && now->hostTime > 0,
        "display link timestamp values");
  dispatch_semaphore_signal((dispatch_semaphore_t)context);
  return kCVReturnSuccess;
}

int main(int argc, char **argv) {
  check(argc == 2 || (argc == 3 && !strcmp(argv[2], "--wayland")), "metallib fixture argument");
  const int wayland = argc == 3;
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    check(device != nil, "Metal device");
    NSUInteger allocated_before = device.currentAllocatedSize;
    MTLTextureDescriptor *volume_descriptor = [[MTLTextureDescriptor alloc] init];
    volume_descriptor.textureType = MTLTextureType3D;
    volume_descriptor.pixelFormat = MTLPixelFormatRGBA8Unorm;
    volume_descriptor.width = 128;
    volume_descriptor.height = 128;
    volume_descriptor.depth = 64;
    volume_descriptor.storageMode = MTLStorageModePrivate;
    volume_descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> volume = [device newTextureWithDescriptor:volume_descriptor];
    check(volume && volume.width == 128 && volume.height == 128 &&
              volume.depth == 64 && volume.textureType == MTLTextureType3D &&
              volume.usage == (MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite),
          "Roblox read/write 128x128x64 private 3D texture");
    check(device.currentAllocatedSize >= allocated_before + 128 * 128 * 64 * 4 &&
              device.recommendedMaxWorkingSetSize > 0,
          "device reports allocated texture memory and working set");
    [volume release];
    [volume_descriptor release];
    check(device.currentAllocatedSize == allocated_before,
          "released volume frees its device allocation");
    MTLTextureDescriptor *packed_descriptor = [[MTLTextureDescriptor alloc] init];
    packed_descriptor.textureType = MTLTextureTypeCube;
    packed_descriptor.pixelFormat = MTLPixelFormatRG11B10Float;
    packed_descriptor.width = 128; packed_descriptor.height = 128;
    packed_descriptor.mipmapLevelCount = 8;
    packed_descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> packed_cube = [device newTextureWithDescriptor:packed_descriptor];
    check(packed_cube && packed_cube.mipmapLevelCount == 8 && packed_cube.pixelFormat == MTLPixelFormatRG11B10Float,
          "Roblox packed-float cube with eight mip levels");
    [packed_cube release]; [packed_descriptor release];
    packed_descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGB9E5Float width:3 height:3 mipmapped:NO];
    packed_descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> packed_shared = [device newTextureWithDescriptor:packed_descriptor];
    check(packed_shared != nil, "RGB9E5 sampled texture does not require a color attachment format");
    [packed_shared release];
    MTLTextureDescriptor *depth_descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:128 height:128 mipmapped:NO];
    depth_descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> depth_texture = [device newTextureWithDescriptor:depth_descriptor];
    check(depth_texture && depth_texture.pixelFormat == MTLPixelFormatDepth32Float,
          "Roblox Depth32Float texture allocation");
    [depth_texture release];
    MTLTextureDescriptor *bc_descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBC1_RGBA width:1024 height:1024 mipmapped:YES];
    bc_descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> bc_texture = [device newTextureWithDescriptor:bc_descriptor];
    check(bc_texture && bc_texture.mipmapLevelCount == 11, "Roblox BC1 1024x1024 texture with full mip chain");
    [bc_texture release];
    MTLSamplerDescriptor *sampler_descriptor = [[MTLSamplerDescriptor alloc] init];
    sampler_descriptor.minFilter = MTLSamplerMinMagFilterLinear;
    sampler_descriptor.magFilter = MTLSamplerMinMagFilterLinear;
    sampler_descriptor.mipFilter = MTLSamplerMipFilterLinear;
    sampler_descriptor.sAddressMode = MTLSamplerAddressModeRepeat;
    sampler_descriptor.lodMinClamp = 0.5f;
    sampler_descriptor.lodMaxClamp = 4.0f;
    check(sampler_descriptor.lodMinClamp == 0.5f &&
              sampler_descriptor.lodMaxClamp == 4.0f, "sampler float property ABI");
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sampler_descriptor];
    check(sampler != nil, "Metal sampler state");
    [sampler release];
    [sampler_descriptor release];
    NSError *error = nil;
    id<MTLLibrary> library =
        [device newLibraryWithFile:[NSString stringWithUTF8String:argv[1]]
                             error:&error];
    if (error)
      fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
    check(library && !error && library.functionNames.count == 16,
          "real Ableton AIR library");
    check([library conformsToProtocol:@protocol(MTLLibrary)],
          "library protocol");
    NSData *file_data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:argv[1]]];
    check(file_data.length > 1, "in-memory metallib fixture");
    size_t split = file_data.length / 2;
    dispatch_data_t first = dispatch_data_create(file_data.bytes, split, NULL, NULL);
    dispatch_data_t second = dispatch_data_create((const char *)file_data.bytes + split,
        file_data.length - split, NULL, NULL);
    dispatch_data_t fragmented = dispatch_data_create_concat(first, second);
    [(id)first release]; [(id)second release];
    id<MTLLibrary> memory_library = [device newLibraryWithData:fragmented error:&error];
    [(id)fragmented release];
    check(memory_library && !error && memory_library.functionNames.count == 16,
        "fragmented dispatch data loads real AIR library");
    [memory_library release];
    dispatch_data_t invalid = dispatch_data_create("bad", 3, NULL, NULL);
    memory_library = [device newLibraryWithData:invalid error:&error];
    [(id)invalid release];
    check(!memory_library && error && [error.domain isEqualToString:MTLLibraryErrorDomain],
        "invalid in-memory metallib returns NSError");
    error = nil;
    id<MTLFunction> vertex = [library newFunctionWithName:@"RectsVertexShader"];
    id<MTLFunction> fragment =
        [library newFunctionWithName:@"IndexedColorFragmentShader"];
    check(vertex && fragment && vertex.functionType == MTLFunctionTypeVertex &&
              fragment.functionType == MTLFunctionTypeFragment,
          "shader function lookup and stage");
    MTLRenderPipelineDescriptor *pd =
        [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = vertex;
    pd.fragmentFunction = fragment;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pd.colorAttachments[0].blendingEnabled = YES;
    pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
    pd.colorAttachments[0].destinationRGBBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;
    pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    pd.colorAttachments[0].destinationAlphaBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;
    check(pd.colorAttachments[0].blendingEnabled, "BOOL descriptor property");
    id<MTLRenderPipelineState> pipeline =
        [device newRenderPipelineStateWithDescriptor:pd error:&error];
    check(pipeline && !error, "Vulkan render pipeline");
    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:64
                                    height:64
                                 mipmapped:NO];
    td.storageMode = MTLStorageModeManaged;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];
    check(target && target.width == 64 && target.height == 64 &&
              target.pixelFormat == MTLPixelFormatBGRA8Unorm,
          "texture descriptor allocation");
    check([target conformsToProtocol:@protocol(MTLTexture)] &&
              [target conformsToProtocol:@protocol(MTLResource)],
          "texture protocols");
    float rect[] = {8, 8, 32, 32}, size[] = {-1, 64, 64},
          color[] = {0.5f, 0, 0, 0.5f};
    uint16_t index[] = {0xffff, 0, 1, 2, 2, 1, 3};
    id<MTLBuffer> rect_buffer =
        [device newBufferWithBytes:rect
                            length:sizeof rect
                           options:MTLResourceStorageModeManaged];
    id<MTLBuffer> size_buffer =
        [device newBufferWithBytes:size
                            length:sizeof size
                           options:MTLResourceStorageModeManaged];
    id<MTLBuffer> indices = [device newBufferWithBytes:index
                                                length:sizeof index
                                               options:0];
    check(rect_buffer && size_buffer && indices &&
              rect_buffer.storageMode == MTLStorageModeManaged,
          "managed buffers");
    [rect_buffer didModifyRange:NSMakeRange(0, sizeof rect)];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    MTLVertexDescriptor *vertex_descriptor = [MTLVertexDescriptor vertexDescriptor];
    vertex_descriptor.attributes[0].format = MTLVertexFormatFloat4;
    vertex_descriptor.attributes[0].offset = 4;
    vertex_descriptor.attributes[0].bufferIndex = 30;
    vertex_descriptor.layouts[30].stride = 20;
    check(vertex_descriptor.attributes[0].format == MTLVertexFormatFloat4 &&
              vertex_descriptor.attributes[0].offset == 4 && vertex_descriptor.attributes[0].bufferIndex == 30 &&
              vertex_descriptor.layouts[30].stride == 20 && vertex_descriptor.layouts[30].stepRate == 1,
          "vertex descriptor arrays and properties");
    MTLDepthStencilDescriptor *depth_state_descriptor = [[MTLDepthStencilDescriptor alloc] init];
    depth_state_descriptor.depthCompareFunction = MTLCompareFunctionAlways;
    depth_state_descriptor.depthWriteEnabled = YES;
    id<MTLDepthStencilState> depth_state = [device newDepthStencilStateWithDescriptor:depth_state_descriptor];
    check(depth_state && depth_state_descriptor.depthCompareFunction == MTLCompareFunctionAlways &&
              depth_state_descriptor.depthWriteEnabled, "depth state creation and descriptor properties");
    const MTLPixelFormat depth_formats[] = {MTLPixelFormatDepth32Float, MTLPixelFormatDepth32Float_Stencil8};
    for (unsigned depth_index = 0; depth_index < 2; ++depth_index) {
    MTLTextureDescriptor *pass_depth_descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:depth_formats[depth_index] width:64 height:64 mipmapped:NO];
    pass_depth_descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    pass_depth_descriptor.storageMode = MTLStorageModeManaged;
    id<MTLTexture> pass_depth_texture = [device newTextureWithDescriptor:pass_depth_descriptor];
    check(pass_depth_texture != nil, "depth render target");
    MTLRenderPipelineDescriptor *depth_pd = [[MTLRenderPipelineDescriptor alloc] init];
    depth_pd.vertexFunction = vertex; depth_pd.fragmentFunction = fragment;
    depth_pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    depth_pd.depthAttachmentPixelFormat = depth_formats[depth_index];
    depth_pd.vertexDescriptor = [MTLVertexDescriptor vertexDescriptor];
    id<MTLRenderPipelineState> depth_pipeline = [device newRenderPipelineStateWithDescriptor:depth_pd error:&error];
    check(depth_pipeline && !error, "pipeline with vertex descriptor and depth format");
    MTLRenderPassDescriptor *depth_pass = pass(target);
    depth_pass.depthAttachment.texture = pass_depth_texture;
    depth_pass.depthAttachment.loadAction = MTLLoadActionClear;
    depth_pass.depthAttachment.storeAction = MTLStoreActionStore;
    depth_pass.depthAttachment.clearDepth = 0.5;
    check(depth_pass.depthAttachment.clearDepth == 0.5, "clearDepth double ABI");
    id<MTLCommandBuffer> depth_command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> depth_encoder = [depth_command renderCommandEncoderWithDescriptor:depth_pass];
    check(depth_encoder != nil, "color plus depth render pass");
    [depth_encoder setRenderPipelineState:depth_pipeline];
    [depth_encoder setDepthStencilState:depth_state];
    [depth_encoder setCullMode:MTLCullModeNone];
    [depth_encoder setFrontFacingWinding:MTLWindingClockwise];
    [depth_encoder setTriangleFillMode:MTLTriangleFillModeFill];
    [depth_encoder setDepthClipMode:MTLDepthClipModeClip];
    [depth_encoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
    [depth_encoder setVertexBuffer:rect_buffer offset:0 atIndex:0];
    [depth_encoder setVertexBuffer:size_buffer offset:4 atIndex:1];
    [depth_encoder setFragmentBytes:color length:sizeof color atIndex:0];
    [depth_encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [depth_encoder endEncoding]; finish(depth_command);
    if (depth_index == 0) {
    float depth_readback[64 * 64];
    [pass_depth_texture getBytes:depth_readback bytesPerRow:64 * 4 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
    check(depth_readback[16 * 64 + 16] != 0.5f && depth_readback[0] == 0.5f,
          "guest depth pass clear, compare and writes produce distinct depth values");
    } else {
      unsigned char combined_pixels[64 * 64 * 4];
      pixels(target, combined_pixels);
      check(combined_pixels[0] == 255 && combined_pixels[(16 * 64 + 16) * 4] != 255,
            "combined depth/stencil texture supports depth rendering and color output");
    }
    [depth_pipeline release]; [depth_pd release]; [pass_depth_texture release];
    }
    [depth_state release]; [depth_state_descriptor release];
    id<MTLCommandBuffer> unretained = [queue commandBufferWithUnretainedReferences];
    check(unretained && !unretained.retainedReferences,
          "unretained-reference command buffer is available");
    finish(unretained);
    uint16_t upload_words[16];
    for (unsigned i = 0; i < 16; ++i) upload_words[i] = (uint16_t)(300 + i);
    id<MTLBuffer> texture_upload = [device newBufferWithBytes:upload_words length:sizeof upload_words options:0];
    MTLTextureDescriptor *integer_descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Uint width:4 height:3 mipmapped:NO];
    integer_descriptor.storageMode = MTLStorageModeManaged;
    id<MTLTexture> integer_texture = [device newTextureWithDescriptor:integer_descriptor];
    check(texture_upload && integer_texture, "R16Uint texture and upload buffer");
    id<MTLCommandBuffer> upload_command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> upload_encoder = [upload_command blitCommandEncoder];
    [upload_encoder copyFromBuffer:texture_upload sourceOffset:4 sourceBytesPerRow:8 sourceBytesPerImage:0
                       sourceSize:MTLSizeMake(4, 3, 1) toTexture:integer_texture destinationSlice:0
                 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [upload_encoder endEncoding];
    finish(upload_command);
    uint16_t downloaded_words[12];
    [integer_texture getBytes:downloaded_words bytesPerRow:8 fromRegion:MTLRegionMake2D(0, 0, 4, 3) mipmapLevel:0];
    check(!memcmp(downloaded_words, upload_words + 2, sizeof downloaded_words),
          "buffer-to-texture blit struct/stack ABI and R16Uint readback");
    [texture_upload release]; [integer_texture release];
    dispatch_semaphore_t started = dispatch_semaphore_create(0);
    dispatch_semaphore_t release_handler = dispatch_semaphore_create(0);
    pthread_t caller = pthread_self();
    __block unsigned async_completed = 0;
    id<MTLCommandBuffer> asynchronous = [queue commandBuffer];
    [asynchronous addCompletedHandler:^(id<MTLCommandBuffer> cb) {
      check(pthread_self() != caller, "completion callback executes on worker");
      dispatch_semaphore_signal(started);
      dispatch_semaphore_wait(release_handler, DISPATCH_TIME_FOREVER);
      [cb waitUntilCompleted]; // reentrant on the completing thread
      ++async_completed;
    }];
    [asynchronous commit];
    check(dispatch_semaphore_wait(started, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)) == 0,
          "commit returns while completion handler is blocked");
    dispatch_semaphore_signal(release_handler);
    [asynchronous waitUntilCompleted];
    check(async_completed == 1, "explicit wait includes asynchronous handlers once");
    dispatch_release(started); dispatch_release(release_handler);
    CVDisplayLinkRef link = NULL;
    check(CVDisplayLinkCreateWithActiveCGDisplays(&link) == kCVReturnSuccess && link,
          "display link creation");
    CVTime nominal = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(link);
    double expected = getenv("MACHOMAN_DISPLAY_HZ") ? atof(getenv("MACHOMAN_DISPLAY_HZ")) : 60;
    check(nominal.timeValue == 1000 && nominal.timeScale == (int)lround(expected * 1000) &&
          nominal.flags == 0, "CVTime two-register return ABI");
    dispatch_semaphore_t ticks = dispatch_semaphore_create(0);
    check(CVDisplayLinkSetOutputCallback(link, display_tick, ticks) == kCVReturnSuccess &&
          CVDisplayLinkStart(link) == kCVReturnSuccess, "display link start");
    check(fabs(CVDisplayLinkGetActualOutputVideoRefreshPeriod(link) - 1.0 / expected) < 1e-8,
          "display link refresh period");
    for (unsigned i = 0; i < 8; ++i)
      check(dispatch_semaphore_wait(ticks, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)) == 0,
            "display link callbacks arrive");
    CVDisplayLinkStop(link); CVDisplayLinkRelease(link); dispatch_release(ticks);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    handlers(command);
    id<MTLRenderCommandEncoder> encoder =
        [command renderCommandEncoderWithDescriptor:pass(target)];
    check(encoder && encoder.device == device &&
              [encoder conformsToProtocol:@protocol(MTLRenderCommandEncoder)],
          "render encoder");
    [encoder setRenderPipelineState:pipeline];
    [encoder setVertexBuffer:rect_buffer offset:0 atIndex:0];
    [encoder setVertexBuffer:size_buffer offset:0 atIndex:1];
    [encoder setVertexBufferOffset:4 atIndex:1];
    [encoder setFragmentBytes:color length:sizeof color atIndex:0];
    [encoder setViewport:(MTLViewport){0, 0, 64, 64, 0, 1}];
    [encoder setScissorRect:(MTLScissorRect){10, 10, 16, 16}];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:6
                         indexType:MTLIndexTypeUInt16
                       indexBuffer:indices
                 indexBufferOffset:2
                     instanceCount:1
                        baseVertex:0
                      baseInstance:0];
    [encoder endEncoding];
    finish(command);
    [command waitUntilCompleted];
    check(scheduled_count == 1 && completed_count == 1,
          "callbacks exactly once");
    unsigned char image[64 * 64 * 4];
    pixels(target, image);
    rectangle(image);
    puts("guest AIR render, managed buffers, descriptor/struct ABI and "
         "callbacks passed");

    // Strided upload uses a 48-byte MTLRegion passed indirectly by arm64.
    unsigned char upload[24] = {255, 0,   0,   255, 0, 255, 0,   255,
                                1,   2,   3,   4,   0, 0,   255, 255,
                                255, 255, 255, 255, 5, 6,   7,   8};
    td.pixelFormat = MTLPixelFormatRGBA8Unorm;
    td.width = 2;
    td.height = 2;
    id<MTLTexture> small = [device newTextureWithDescriptor:td];
    [small replaceRegion:MTLRegionMake2D(0, 0, 2, 2)
             mipmapLevel:0
               withBytes:upload
             bytesPerRow:12];
    unsigned char copy[16] = {0};
    [small getBytes:copy
        bytesPerRow:8
         fromRegion:MTLRegionMake2D(0, 0, 2, 2)
        mipmapLevel:0];
    check(!memcmp(copy, upload, 8) && !memcmp(copy + 8, upload + 12, 8),
          "texture region and row stride ABI");
    [small release];

    id<MTLCommandBuffer> bad = [queue commandBuffer];
    [bad addCompletedHandler:^(id<MTLCommandBuffer> cb) {
      check(cb.status == MTLCommandBufferStatusError && cb.error,
            "failed batch callback");
      ++failed_count;
    }];
    MTLRenderPassDescriptor *bp = pass(target);
    bp.colorAttachments[0].clearColor = MTLClearColorMake(0, 1, 0, 1);
    encoder = [bad renderCommandEncoderWithDescriptor:bp];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                vertexStart:0
                vertexCount:4];
    [encoder endEncoding];
    [bad commit];
    [bad waitUntilCompleted];
    check(failed_count == 1 && bad.error, "failed callback once");
    pixels(target, image);
    rectangle(image);

    [NSApplication sharedApplication];
    NSWindow *window =
        [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 64, 64)
                                    styleMask:wayland ? 0 : NSWindowStyleMaskTitled
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    MEVKTestView *view =
        [[MEVKTestView alloc] initWithFrame:NSMakeRect(0, 0, 64, 64)];
    window.contentView = view;
    view.layerContentsPlacement = NSViewLayerContentsPlacementTopLeft;
    view.wantsLayer = YES;
    CAMetalLayer *layer = (CAMetalLayer *)view.layer;
    check([layer isKindOfClass:[CAMetalLayer class]],
          "AppKit Metal backing layer");
    check(view.layerContentsPlacement == NSViewLayerContentsPlacementTopLeft &&
              [layer.contentsGravity isEqual:@"topLeft"], "placement set before layer creation");
    view.layerContentsPlacement = NSViewLayerContentsPlacementCenter;
    check([layer.contentsGravity isEqual:@"center"], "placement updates existing backing layer");
    layer.contentsGravity = @"resizeAspect";
    check([layer.contentsGravity isEqual:@"resizeAspect"], "layer gravity property");
    view.layerContentsPlacement = NSViewLayerContentsPlacementTopLeft;
    view.frame = NSMakeRect(11, 22, 356, 356);
    CGRect resized_frame = layer.frame;
    check(resized_frame.origin.x == 0 && resized_frame.origin.y == 0 &&
              resized_frame.size.width == 356 && resized_frame.size.height == 356 &&
              layer.bounds.size.width == 356 && layer.bounds.size.height == 356,
          "resized view updates Metal layer frame as well as bounds");
    view.frame = NSMakeRect(0, 0, 64, 64);
    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.drawableSize = CGSizeMake(64, 64);
    layer.framebufferOnly = NO;
    CGSize drawable_size = layer.drawableSize;
    check(drawable_size.width == 64 && drawable_size.height == 64,
          "drawable CGSize ABI");
    if (!wayland) [window orderFront:nil];
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    check(drawable && drawable.texture && drawable.layer == layer,
          "nextDrawable");
    command = [queue commandBuffer];
    encoder =
        [command renderCommandEncoderWithDescriptor:pass(drawable.texture)];
    [encoder endEncoding];
    [command presentDrawable:drawable];
    [command commit];
    if (wayland) [window orderFront:nil]; // Race initial presentation against ordering.
    [command waitUntilCompleted];
    check(command.status == MTLCommandBufferStatusCompleted && !command.error,
          "first presentation completes while window is ordered");
    pixels(drawable.texture, image);
    check(image[0] == 255 && image[1] == 0 && image[2] == 0 && image[3] == 255,
          "presented Vulkan clear");
    @autoreleasepool {
      id<CAMetalDrawable> other = [layer nextDrawable];
      check(other != nil, "second drawable while first is retained");
      MTLRenderPassDescriptor *red = pass(other.texture);
      red.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
      id<MTLCommandBuffer> reuse = [queue commandBuffer];
      encoder = [reuse renderCommandEncoderWithDescriptor:red];
      [encoder endEncoding]; finish(reuse);
      pixels(drawable.texture, image);
      check(image[0] == 255 && image[2] == 0, "pool overwrote a retained drawable");
    }
    [window close];
    if (wayland) {
      // A real WSI probe: blue content with yellow drawable padding. The
      // visible 356x356 window must contain only blue, including its edges.
      const unsigned visible_width = getenv("MEVK_PROBE_LARGE") ? 1908 : 356;
      const unsigned visible_height = getenv("MEVK_PROBE_LARGE") ? 1000 : 356;
      const unsigned texture_width = getenv("MEVK_PROBE_LARGE") ? 2048 : 512;
      const unsigned texture_height = getenv("MEVK_PROBE_LARGE") ? 1024 : 512;
      NSWindow *probe_window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, visible_width, visible_height)
          styleMask:0 backing:NSBackingStoreBuffered defer:NO];
      probe_window.title = @"MEVK WSI crop probe";
      MEVKTestView *probe = [[MEVKTestView alloc] initWithFrame:NSMakeRect(0, 0, visible_width, visible_height)];
      probe_window.contentView = probe; probe.layerContentsPlacement = NSViewLayerContentsPlacementTopLeft;
      probe.wantsLayer = YES;
      CAMetalLayer *probe_layer = (CAMetalLayer *)probe.layer;
      probe_layer.device = device; probe_layer.drawableSize = CGSizeMake(texture_width, texture_height);
      probe_layer.pixelFormat = MTLPixelFormatBGRA8Unorm; probe_layer.framebufferOnly = NO;
      [probe_window orderFront:nil];
      uint32_t *pattern = malloc(texture_width * texture_height * 4);
      for (unsigned y = 0; y < texture_height; ++y) for (unsigned x = 0; x < texture_width; ++x)
        pattern[y * texture_width + x] = x < visible_width && y < visible_height ? 0xff0000ff : 0xffffff00;
      const unsigned frames = getenv("MEVK_PROBE_FRAMES") ? (unsigned)atoi(getenv("MEVK_PROBE_FRAMES")) : 180;
      double present_total = 0, present_max = 0;
      for (unsigned frame = 0; frame < frames; ++frame) {
        @autoreleasepool {
          id<CAMetalDrawable> next = [probe_layer nextDrawable];
          check(next != nil, "pooled WSI drawable remains available");
          [next.texture replaceRegion:MTLRegionMake2D(0, 0, texture_width, texture_height) mipmapLevel:0 withBytes:pattern bytesPerRow:texture_width * 4];
          id<MTLCommandBuffer> cb = [queue commandBuffer]; [cb presentDrawable:next];
          uint64_t begin = CVGetCurrentHostTime(); finish(cb);
          double ms = (CVGetCurrentHostTime() - begin) / 1e6;
          if (frame >= 10) { present_total += ms; if (ms > present_max) present_max = ms; }
          NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil
              inMode:NSDefaultRunLoopMode dequeue:YES];
          if (event) [NSApp sendEvent:event];
          usleep(16666);
        }
      }
      printf("MEVK WSI benchmark: frames=%u commit+wait_mean_ms=%.3f max_ms=%.3f\n",
             frames, frames > 10 ? present_total / (frames - 10) : 0, present_max);
      free(pattern); [probe_window close]; [probe release]; [probe_window release];
    }
    [view release];
    [window release];
    [indices release];
    [size_buffer release];
    [rect_buffer release];
    [target release];
    [pipeline release];
    [pd release];
    [fragment release];
    [vertex release];
    [library release];
    [queue release];
    [device release];
    puts("MEVK Metal guest render and AppKit drawable passed");
  }
  return 0;
}
