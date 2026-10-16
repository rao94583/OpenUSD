#import "movie_writer.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <unistd.h>

@implementation VelaMovieWriter {
    AVAssetWriter *_writer;
    AVAssetWriterInput *_input;
    AVAssetWriterInputPixelBufferAdaptor *_adaptor;
    NSURL *_url;
    int _width, _height;
    double _framesPerSecond;
    int64_t _frame;
}

- (instancetype)initWithPath:(NSString *)path width:(int)width height:(int)height
             framesPerSecond:(double)framesPerSecond codec:(NSString *)codec
                       error:(NSString **)error {
    if (!(self = [super init])) return nil;
    _url = [NSURL fileURLWithPath:path];
    _width = width;
    _height = height;
    _framesPerSecond = framesPerSecond;
    [NSFileManager.defaultManager removeItemAtURL:_url error:nil];
    NSDictionary<NSString *, AVVideoCodecType> *codecs = @{
        @"h264": AVVideoCodecTypeH264, @"hevc": AVVideoCodecTypeHEVC,
        @"prores422": AVVideoCodecTypeAppleProRes422,
        @"prores4444": AVVideoCodecTypeAppleProRes4444,
    };
    AVVideoCodecType type = codecs[codec];
    if (!type) {
        if (error) *error = [NSString stringWithFormat:@"Unknown codec %@", codec];
        return nil;
    }
    AVFileType fileType = [path.pathExtension.lowercaseString isEqualToString:@"mp4"]
        ? AVFileTypeMPEG4 : AVFileTypeQuickTimeMovie;
    NSError *failure = nil;
    _writer = [AVAssetWriter assetWriterWithURL:_url fileType:fileType error:&failure];
    if (!_writer) {
        if (error) *error = failure.localizedDescription;
        return nil;
    }
    NSMutableDictionary *settings = [@{
        AVVideoCodecKey: type, AVVideoWidthKey: @(width), AVVideoHeightKey: @(height),
    } mutableCopy];
    if (type == AVVideoCodecTypeH264 || type == AVVideoCodecTypeHEVC) {
        // High quality: about 0.2 bits per pixel per frame, at least 8 Mbit/s.
        const double bitsPerSecond = std::max(8e6, 0.2 * width * height * framesPerSecond);
        settings[AVVideoCompressionPropertiesKey] = @{AVVideoAverageBitRateKey: @(bitsPerSecond)};
    }
    if (![_writer canApplyOutputSettings:settings forMediaType:AVMediaTypeVideo]) {
        if (error) *error = [NSString stringWithFormat:@"%@ can't encode %d x %d %@",
                             codec, width, height, path.pathExtension];
        return nil;
    }
    _input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:settings];
    _input.expectsMediaDataInRealTime = NO;
    _adaptor = [AVAssetWriterInputPixelBufferAdaptor
        assetWriterInputPixelBufferAdaptorWithAssetWriterInput:_input
        sourcePixelBufferAttributes:@{
            (NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
            (NSString *)kCVPixelBufferWidthKey: @(width),
            (NSString *)kCVPixelBufferHeightKey: @(height)}];
    [_writer addInput:_input];
    if (![_writer startWriting]) {
        if (error) *error = _writer.error.localizedDescription ?: @"Could not start writing";
        return nil;
    }
    [_writer startSessionAtSourceTime:kCMTimeZero];
    return self;
}

- (BOOL)appendFrame:(const uint8_t *)rgba error:(NSString **)error {
    // The encoder takes frames as it's ready for them.
    while (!_input.readyForMoreMediaData) {
        if (_writer.status == AVAssetWriterStatusFailed) break;
        usleep(2000);
    }
    CVPixelBufferRef buffer = nullptr;
    if (!_adaptor.pixelBufferPool ||
        CVPixelBufferPoolCreatePixelBuffer(nullptr, _adaptor.pixelBufferPool, &buffer) != kCVReturnSuccess) {
        if (error) *error = _writer.error.localizedDescription ?: @"No pixel buffer for the frame";
        return NO;
    }
    CVPixelBufferLockBaseAddress(buffer, 0);
    uint8_t *base = static_cast<uint8_t *>(CVPixelBufferGetBaseAddress(buffer));
    const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
    for (int y = 0; y < _height; ++y) {
        const uint8_t *from = rgba + size_t(y) * _width * 4;
        uint8_t *to = base + size_t(y) * stride;
        for (int x = 0; x < _width; ++x, from += 4, to += 4) {
            to[0] = from[2]; to[1] = from[1]; to[2] = from[0]; to[3] = 255;
        }
    }
    CVPixelBufferUnlockBaseAddress(buffer, 0);
    // Frame times at the movie's rate, exact for rates like 23.976.
    const CMTime time = CMTimeMakeWithSeconds(double(_frame) / _framesPerSecond, 90000);
    const BOOL ok = [_adaptor appendPixelBuffer:buffer withPresentationTime:time];
    CVPixelBufferRelease(buffer);
    if (!ok) {
        if (error) *error = _writer.error.localizedDescription ?: @"Could not add the frame";
        return NO;
    }
    ++_frame;
    return YES;
}

- (BOOL)finish:(NSString **)error {
    [_input markAsFinished];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [_writer finishWritingWithCompletionHandler:^{ dispatch_semaphore_signal(done); }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (_writer.status != AVAssetWriterStatusCompleted) {
        if (error) *error = _writer.error.localizedDescription ?: @"The movie was not completed";
        return NO;
    }
    return YES;
}

- (void)cancel {
    [_writer cancelWriting];
    [NSFileManager.defaultManager removeItemAtURL:_url error:nil];
}

@end
