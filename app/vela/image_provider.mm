#import "image_provider.h"
#import <ImageIO/ImageIO.h>
#import <Security/Security.h>
#import <CommonCrypto/CommonDigest.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include "app/vela/core/mcp.h"
#include "pxr/base/js/json.h"

using namespace vela::images;
PXR_NAMESPACE_USING_DIRECTIVE
namespace {
Bytes DataBytes(NSData *d) { if (!d) return {}; const uint8_t *p = (const uint8_t *)d.bytes; return Bytes(p,p+d.length); }
NSData *Data(const Bytes &b) { return [NSData dataWithBytes:b.data() length:b.size()]; }
NSData *PNG(CGImageRef image) {
    if (!image) return nil;
    NSMutableData *data = [NSMutableData data];
    CGImageDestinationRef dest = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)data, CFSTR("public.png"),1,nullptr);
    if (!dest) return nil;
    CGImageDestinationAddImage(dest,image,nullptr);
    bool ok = CGImageDestinationFinalize(dest); CFRelease(dest); return ok ? data : nil;
}
NSDictionary *KeyQuery(NSString *connection) {
    return @{(__bridge id)kSecClass:(__bridge id)kSecClassGenericPassword,
             (__bridge id)kSecAttrService:@"org.openusd.vela.images",
             (__bridge id)kSecAttrAccount:connection};
}
NSString *Encode(const Bytes &b) { return [Data(b) base64EncodedStringWithOptions:0]; }
}
NSString *VelaImageKey(NSString *connection) {
    NSMutableDictionary *query = [KeyQuery(connection) mutableCopy];
    query[(__bridge id)kSecReturnData] = @YES;
    query[(__bridge id)kSecUseAuthenticationUI] = (__bridge id)kSecUseAuthenticationUIFail;
    CFTypeRef value = nullptr;
    if (SecItemCopyMatching((__bridge CFDictionaryRef)query,&value) != errSecSuccess) return @"";
    NSData *data = CFBridgingRelease(value);
    return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"";
}
bool VelaSetImageKey(NSString *connection, NSString *key) {
    NSDictionary *query = KeyQuery(connection);
    if (!key.length) { OSStatus s = SecItemDelete((__bridge CFDictionaryRef)query); return s == errSecSuccess || s == errSecItemNotFound; }
    NSDictionary *update = @{(__bridge id)kSecValueData:[key dataUsingEncoding:NSUTF8StringEncoding]};
    OSStatus status = SecItemUpdate((__bridge CFDictionaryRef)query,(__bridge CFDictionaryRef)update);
    if (status == errSecItemNotFound) {
        NSMutableDictionary *item = [query mutableCopy]; [item addEntriesFromDictionary:update];
        item[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
        status = SecItemAdd((__bridge CFDictionaryRef)item,nullptr);
    }
    return status == errSecSuccess;
}
Output VelaNormalizeImage(NSData *data) {
    Output out;
    if (!data.length || data.length > 32*1024*1024) { out.error = "invalid_output: image byte limit"; return out; }
    CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)data,nullptr);
    if (!src) { out.error = "invalid_output: not an image"; return out; }
    NSDictionary *properties = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(src,0,nullptr));
    const int64_t width = [properties[(__bridge id)kCGImagePropertyPixelWidth] longLongValue];
    const int64_t height = [properties[(__bridge id)kCGImagePropertyPixelHeight] longLongValue];
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || width*height > 16777216 || CGImageSourceGetCount(src) != 1) {
        CFRelease(src); out.error = "invalid_output: image dimensions or frame count"; return out;
    }
    CGImageRef image = CGImageSourceCreateImageAtIndex(src,0,nullptr);
    if (!image) { CFRelease(src); out.error = "invalid_output: cannot decode image"; return out; }
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(nullptr,width,height,8,0,space,kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(space);
    if (ctx) {
        CGContextDrawImage(ctx,CGRectMake(0,0,width,height),image);
        CGImageRef normalized = CGBitmapContextCreateImage(ctx);
        out.png = DataBytes(PNG(normalized)); CGImageRelease(normalized); CGContextRelease(ctx);
    }
    CGImageRef thumb = CGImageSourceCreateThumbnailAtIndex(src,0,(__bridge CFDictionaryRef)@{
        (__bridge id)kCGImageSourceCreateThumbnailFromImageAlways:@YES,
        (__bridge id)kCGImageSourceThumbnailMaxPixelSize:@512,
        (__bridge id)kCGImageSourceCreateThumbnailWithTransform:@YES});
    if (thumb) { out.preview = DataBytes(PNG(thumb)); CGImageRelease(thumb); }
    CGImageRelease(image); CFRelease(src);
    out.width = int(width); out.height = int(height);
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(out.png.data(),CC_LONG(out.png.size()),digest);
    const char hex[]="0123456789abcdef";
    for (unsigned char b : digest) { out.sha256 += hex[b >> 4]; out.sha256 += hex[b & 15]; }
    if (out.png.empty() || out.preview.empty()) out.error = "invalid_output: cannot normalize image";
    return out;
}
NSMutableURLRequest *VelaImageRequest(const Request &r, NSString *key) {
    const bool openai = r.model.provider == "openai";
    NSString *url = openai ? (r.references.empty() ? @"https://api.openai.com/v1/images/generations" : @"https://api.openai.com/v1/images/edits")
        : [NSString stringWithFormat:@"https://generativelanguage.googleapis.com/v1beta/models/%@:generateContent",@(r.model.name.c_str())];
    NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:url]];
    req.HTTPMethod = @"POST"; req.timeoutInterval = 300;
    [req setValue:openai ? [@"Bearer " stringByAppendingString:key] : key
        forHTTPHeaderField:openai ? @"Authorization" : @"x-goog-api-key"];
    if (openai && !r.references.empty()) {
        NSString *boundary = NSUUID.UUID.UUIDString;
        [req setValue:[@"multipart/form-data; boundary=" stringByAppendingString:boundary] forHTTPHeaderField:@"Content-Type"];
        NSMutableData *body = [NSMutableData data];
        auto append = [&](NSString *s) { [body appendData:[s dataUsingEncoding:NSUTF8StringEncoding]]; };
        for (const char *field : {"model","prompt","size","quality","background"}) {
            append([NSString stringWithFormat:@"--%@\r\nContent-Disposition: form-data; name=\"%@\"\r\n\r\n%@\r\n",boundary,@(field),@(Text(r.options,field).c_str())]);
        }
        append([NSString stringWithFormat:@"--%@\r\nContent-Disposition: form-data; name=\"output_format\"\r\n\r\npng\r\n",boundary]);
        for (const auto &b : r.references) {
            append([NSString stringWithFormat:@"--%@\r\nContent-Disposition: form-data; name=\"image[]\"; filename=\"reference.png\"\r\nContent-Type: image/png\r\n\r\n",boundary]);
            [body appendData:Data(b)]; append(@"\r\n");
        }
        append([NSString stringWithFormat:@"--%@--\r\n",boundary]); req.HTTPBody = body;
    } else {
        NSMutableDictionary *body = [NSMutableDictionary dictionary];
        if (openai) {
            for (const char *f : {"model","prompt","size","quality","background"}) body[@(f)] = @(Text(r.options,f).c_str());
            body[@"n"] = @1; body[@"output_format"] = @"png";
        } else {
            NSMutableArray *parts = [NSMutableArray arrayWithObject:@{@"text":@(Text(r.options,"prompt").c_str())}];
            for (const auto &b : r.references) [parts addObject:@{@"inlineData":@{@"mimeType":@"image/png",@"data":Encode(b)}}];
            body[@"contents"] = @[@{@"role":@"user",@"parts":parts}];
            body[@"generationConfig"] = @{@"responseModalities":@[@"TEXT",@"IMAGE"],@"imageConfig":@{@"aspectRatio":@"1:1"}};
        }
        req.HTTPBody = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
        [req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    }
    return req;
}
Output VelaDecodeImageResponse(NSString *provider, NSData *data) {
    Output out;
    id parsed = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![parsed isKindOfClass:NSDictionary.class]) { out.error = "invalid_output: malformed response"; return out; }
    NSDictionary *json = parsed;
    NSString *b64 = nil;
    if ([provider isEqualToString:@"openai"]) {
        NSArray *items = json[@"data"];
        if ([items isKindOfClass:NSArray.class] && items.count && [items[0] isKindOfClass:NSDictionary.class]) b64 = items[0][@"b64_json"];
    } else {
        id candidates = json[@"candidates"];
        if ([candidates isKindOfClass:NSArray.class]) for (id candidate in candidates) {
            if (![candidate isKindOfClass:NSDictionary.class]) continue;
            id content = candidate[@"content"]; if (![content isKindOfClass:NSDictionary.class]) continue;
            id parts = content[@"parts"]; if (![parts isKindOfClass:NSArray.class]) continue;
            for (id part in parts) if ([part isKindOfClass:NSDictionary.class]) {
                id inlineData = part[@"inlineData"] ?: part[@"inline_data"];
                if ([inlineData isKindOfClass:NSDictionary.class]) b64 = inlineData[@"data"];
            }
        }
    }
    if (![b64 isKindOfClass:NSString.class] || b64.length > 48*1024*1024) { out.error = "no_image: provider returned no image (possibly rejected)"; return out; }
    out = VelaNormalizeImage([[NSData alloc] initWithBase64EncodedString:b64 options:0]);
    id usage = json[@"usage"] ?: json[@"usageMetadata"];
    if ([usage isKindOfClass:NSDictionary.class]) {
        NSData *encoded = [NSJSONSerialization dataWithJSONObject:usage options:0 error:nil];
        JsValue v = JsParseString(std::string((const char *)encoded.bytes,encoded.length)); if (v.IsObject()) out.usage = v.GetJsObject();
    }
    return out;
}

// Stream into a bounded buffer. Reject redirects so credentials never change host.
@interface VelaImageTransfer : NSObject <NSURLSessionDataDelegate>
@property(nonatomic, strong) NSMutableData *bytes;
@property(nonatomic, strong) NSURLSession *session;
@property(nonatomic, copy) void (^completion)(NSData *, NSInteger, NSString *, NSString *);
@property(nonatomic) NSInteger status;
@property(nonatomic, copy) NSString *requestId;
@end
@implementation VelaImageTransfer
- (void)URLSession:(NSURLSession *)s task:(NSURLSessionTask *)task willPerformHTTPRedirection:(NSHTTPURLResponse *)r newRequest:(NSURLRequest *)req completionHandler:(void (^)(NSURLRequest *))done { done(nil); }
- (void)URLSession:(NSURLSession *)s dataTask:(NSURLSessionDataTask *)task didReceiveResponse:(NSURLResponse *)r completionHandler:(void (^)(NSURLSessionResponseDisposition))done {
    self.status = ((NSHTTPURLResponse *)r).statusCode;
    self.requestId = [((NSHTTPURLResponse *)r) valueForHTTPHeaderField:@"x-request-id"] ?: @"";
    done(r.expectedContentLength > 64*1024*1024 ? NSURLSessionResponseCancel : NSURLSessionResponseAllow);
}
- (void)URLSession:(NSURLSession *)s dataTask:(NSURLSessionDataTask *)task didReceiveData:(NSData *)data {
    if (self.bytes.length + data.length > 64*1024*1024) [task cancel]; else [self.bytes appendData:data];
}
- (void)URLSession:(NSURLSession *)s task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error {
    NSString *failure = error ? (error.code == NSURLErrorTimedOut ? @"timeout: outcome unknown; retry manually" : @"transport: request failed or cancelled; outcome unknown") : @"";
    self.completion(self.bytes,self.status,self.requestId,failure);
    self.completion = nil; [s finishTasksAndInvalidate]; self.session = nil;
}
@end
void VelaRegisterImageProviders() {
    for (const Model &model : std::vector<Model>{
        {"openai","openai","gpt-image-2.5-flare","api.openai.com",3,{"1024x1024","1536x1024","1024x1536"},{"medium","low","high"},{"opaque","transparent","auto"}},
        {"gemini","gemini","gemini-2.5-flash-image","generativelanguage.googleapis.com",3,{"1024x1024"},{"auto"},{"opaque"}}}) {
        Provider p; p.model = model;
        p.ready = [connection=model.connection] { return VelaImageKey(@(connection.c_str())).length > 0; };
        p.submit = [](Request r, std::function<void(Output)> complete) {
            VelaImageTransfer *transfer = [VelaImageTransfer new]; transfer.bytes = [NSMutableData data];
            NSString *provider = @(r.model.provider.c_str());
            transfer.completion = ^(NSData *bytes, NSInteger status, NSString *rid, NSString *failure) {
                Output out;
                if (failure.length) out.error = failure.UTF8String;
                else if (status < 200 || status >= 300) {
                    const std::string type = status == 401 || status == 403 ? "authentication_or_access" : status == 429 ? "quota_or_rate_limit" : status == 400 ? "invalid_request_or_rejected" : "provider_http_error";
                    out.error = type + ": HTTP " + std::to_string(status) + "; no automatic retry";
                } else out = VelaDecodeImageResponse(provider,bytes);
                out.requestId = (rid ?: @"").UTF8String;
                dispatch_async(dispatch_get_main_queue(), ^{ complete(out); });
            };
            NSURLSessionConfiguration *config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
            config.timeoutIntervalForResource = 300; config.URLCache = nil; config.HTTPShouldSetCookies = NO;
            transfer.session = [NSURLSession sessionWithConfiguration:config delegate:transfer delegateQueue:nil];
            NSURLSessionDataTask *task = [transfer.session dataTaskWithRequest:VelaImageRequest(r,VelaImageKey(@(r.model.connection.c_str())))];
            [task resume];
            return std::function<void()>([task] { [task cancel]; });
        };
        Service::Get().Register(std::move(p));
    }
}
