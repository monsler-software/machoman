#import <AppKit/AppKit.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
extern int machoman_bitmap_check(id,const uint32_t *,int,int);
static void require(BOOL ok,const char *s){if(!ok){fprintf(stderr,"FAIL: %s\n",s);exit(1);}}
@interface RasterView:NSView {
@public NSImage *image;BOOL flipped;int phase;
}
@end
@implementation RasterView
- (BOOL)isFlipped{return flipped;}
- (void)drawRect:(NSRect)rect{
    [[NSColor blueColor]setFill];NSRectFill(self.bounds);
    if(phase==0)[image drawInRect:NSMakeRect(0,0,2,2) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:.5];
    else [image drawInRect:NSMakeRect(1,1,2,2) fromRect:NSMakeRect(0,1,2,1) operation:NSCompositingOperationCopy fraction:1];
}
@end
int main(){@autoreleasepool{
    NSBitmapImageRep *rep=[[NSBitmapImageRep alloc]initWithBitmapDataPlanes:NULL pixelsWide:2 pixelsHigh:2 bitsPerSample:8 samplesPerPixel:3 hasAlpha:NO isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:32];
    require(rep&&rep.bytesPerRow==8,"RGB32 buffer");
    unsigned char pixels[]={255,0,0,0,255,0,0,0,0,255,0,0,0,255,0,0};memcpy(rep.bitmapData,pixels,sizeof pixels);
    RasterView *v=[[RasterView alloc]initWithFrame:NSMakeRect(0,0,4,4)];v->image=[[NSImage alloc]initWithSize:NSMakeSize(2,2)];[v->image addRepresentation:rep];[rep release];
    [v display];uint32_t expected[]={0xff0000ff,0xff0000ff,0xff0000ff,0xff0000ff,0xff0000ff,0xff0000ff,0xff0000ff,0xff0000ff,0xff80007f,0xff80007f,0xff0000ff,0xff0000ff,0xff00807f,0xff00807f,0xff0000ff,0xff0000ff};
    require(!machoman_bitmap_check(v,expected,4,4),"source-over alpha and bottom-left view coordinates");
    v->flipped=YES;[v setNeedsDisplay:YES];[v displayIfNeeded];
    uint32_t flipped[]={0xff80007f,0xff80007f,0xff0000ff,0xff0000ff,0xff00807f,0xff00807f,0xff0000ff,0xff0000ff,0xff0000ff,0xff0000ff,0xff0000ff,0xff0000ff,0xff0000ff,0xff0000ff,0xff0000ff,0xff0000ff};
    require(!machoman_bitmap_check(v,flipped,4,4),"flipped coordinates and dirty bitmap replacement");
    v->phase=1;[v display];uint32_t cropped[]={0xff0000ff,0xff0000ff,0xff0000ff,0xff0000ff,0xff0000ff,0xffff0000,0xffff0000,0xff0000ff,0xff0000ff,0xffff0000,0xffff0000,0xff0000ff,0xff0000ff,0xff0000ff,0xff0000ff,0xff0000ff};
    require(!machoman_bitmap_check(v,cropped,4,4),"Cocoa source crop and image scaling");
    [v->image release];[v release];puts("Cocoa ARM64 bitmap drawing, alpha, orientation and cropping passed");
}return 0;}
