// A sheet that asks for an action's form fields (see vela::Action::form):
// a checkbox, number or text field, or pop-up per field.
#pragma once

#import <Cocoa/Cocoa.h>

#include "app/vela/core/actions.h"

#include <vector>

// Shows the form on window (or as a modal panel without one). On OK,
// completion receives every field's value by key: bools, int64s, doubles
// or strings. Number fields that do not parse keep their previous value.
void VelaRunForm(NSWindow *window, NSString *title,
                 const std::vector<vela::FormField> &fields,
                 void (^completion)(const PXR_NS::JsObject &values));
