// Encodes rendered frames into a QuickTime or MPEG-4 movie with
// AVFoundation: H.264, HEVC or ProRes.
#import <Foundation/Foundation.h>

@interface VelaMovieWriter : NSObject
// codec is h264, hevc, prores422 or prores4444; the file type follows the
// path's extension (.mov or .mp4). Width and height must be even for H.264
// and HEVC. Replaces an existing file.
- (instancetype)initWithPath:(NSString *)path width:(int)width height:(int)height
             framesPerSecond:(double)framesPerSecond codec:(NSString *)codec
                       error:(NSString **)error;
// RGBA, 8 bits per channel, top row first, width * height pixels.
- (BOOL)appendFrame:(const uint8_t *)rgba error:(NSString **)error;
// Finishes the file and waits for it to be written.
- (BOOL)finish:(NSString **)error;
// Stops and removes the partial file.
- (void)cancel;
@end
