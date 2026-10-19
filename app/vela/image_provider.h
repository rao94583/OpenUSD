#pragma once
#import <Foundation/Foundation.h>
#include "app/vela/core/image_jobs.h"
// Testable wire adapters and image validation; never log a returned request.
NSMutableURLRequest *VelaImageRequest(const vela::images::Request &, NSString *key);
vela::images::Output VelaDecodeImageResponse(NSString *provider, NSData *data);
vela::images::Output VelaNormalizeImage(NSData *data);
NSString *VelaImageKey(NSString *connection);
bool VelaSetImageKey(NSString *connection, NSString *key);
void VelaRegisterImageProviders();
