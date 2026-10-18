#import "image_provider.h"
#include "app/vela/core/test_support.h"
using namespace vela::images;
PXR_NAMESPACE_USING_DIRECTIVE
int ImageProviderFixtures() { @autoreleasepool {
    Request r; r.model={"openai","openai","gpt-image-2.5-flare","api.openai.com",3,{},{},{}};
    r.options={{"prompt",JsValue("fabric")},{"model",JsValue(r.model.name)},{"size",JsValue("1024x1024")},{"quality",JsValue("medium")},{"background",JsValue("opaque")}};
    auto request=VelaImageRequest(r,@"test-key");
    CHECK([request.URL.absoluteString isEqualToString:@"https://api.openai.com/v1/images/generations"]);
    CHECK([[request valueForHTTPHeaderField:@"Authorization"] isEqualToString:@"Bearer test-key"]);
    NSDictionary *body=[NSJSONSerialization JSONObjectWithData:request.HTTPBody options:0 error:nil];
    CHECK([body[@"prompt"] isEqualToString:@"fabric"] && [body[@"output_format"] isEqualToString:@"png"]);
    r.references={{1,2,3}}; request=VelaImageRequest(r,@"test-key");
    CHECK([request.URL.path isEqualToString:@"/v1/images/edits"]);
    NSString *multipart=[[NSString alloc] initWithData:request.HTTPBody encoding:NSUTF8StringEncoding];
    CHECK([multipart containsString:@"name=\"image[]\"; filename=\"reference.png\""]);
    CHECK([multipart containsString:@"name=\"prompt\"\r\n\r\nfabric\r\n"]);
    r.model.provider="gemini"; r.model.name="gemini-2.5-flash-image";
    request=VelaImageRequest(r,@"google-key");
    CHECK([request.URL.host isEqualToString:@"generativelanguage.googleapis.com"]);
    CHECK([[request valueForHTTPHeaderField:@"x-goog-api-key"] isEqualToString:@"google-key"]);
    CHECK(![request valueForHTTPHeaderField:@"Authorization"]);
    body=[NSJSONSerialization JSONObjectWithData:request.HTTPBody options:0 error:nil];
    CHECK([body[@"contents"][0][@"parts"][1][@"inlineData"][@"data"] isEqualToString:@"AQID"]);
    // Real PNG bytes exercise ImageIO validation and normalization.
    NSString *base=@"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aI1sAAAAASUVORK5CYII=";
    NSData *png=[[NSData alloc] initWithBase64EncodedString:base options:0];
    Output normalized=VelaNormalizeImage(png); CHECK(normalized.error.empty());
    CHECK(normalized.width==1 && normalized.height==1 && !normalized.preview.empty());
    NSData *openai=[NSJSONSerialization dataWithJSONObject:@{@"data":@[@{@"b64_json":base}],@"usage":@{@"total_tokens":@1}} options:0 error:nil];
    auto output=VelaDecodeImageResponse(@"openai",openai);
    CHECK(output.error.empty() && output.width==1 && !output.usage.empty());
    NSData *gemini=[NSJSONSerialization dataWithJSONObject:@{@"candidates":@[@{@"content":@{@"parts":@[@{@"inlineData":@{@"mimeType":@"image/png",@"data":base}}]}}]} options:0 error:nil];
    CHECK(VelaDecodeImageResponse(@"gemini",gemini).error.empty());
    for (NSString *invalid in @[@"not json",@"[]",@"{\"data\":true}",@"{\"candidates\":[false]}",@"{\"data\":[{\"b64_json\":false}]}"]) {
        CHECK(!VelaDecodeImageResponse(@"openai",[invalid dataUsingEncoding:NSUTF8StringEncoding]).error.empty());
        CHECK(!VelaDecodeImageResponse(@"gemini",[invalid dataUsingEncoding:NSUTF8StringEncoding]).error.empty());
    }
    CHECK(!VelaNormalizeImage([@"not an image" dataUsingEncoding:NSUTF8StringEncoding]).error.empty());
    return vela::test::Finish("image provider fixtures");
} }
