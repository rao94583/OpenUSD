#import "form_sheet.h"

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

NSString *Text(const JsValue &value) {
    if (value.IsString()) return @(value.GetString().c_str());
    if (value.IsInt()) return [NSString stringWithFormat:@"%lld", (long long)value.GetInt64()];
    if (value.IsUInt64()) return [NSString stringWithFormat:@"%llu",
                                  (unsigned long long)value.GetUInt64()];
    if (value.IsReal()) return [NSString stringWithFormat:@"%.10g", value.GetReal()];
    return @"";
}

NSView *Control(const vela::FormField &field) {
    using T = vela::FormField::Type;
    switch (field.type) {
    case T::Bool: {
        NSButton *box = [NSButton checkboxWithTitle:@"" target:nil action:nil];
        box.state = field.value.IsBool() && field.value.GetBool()
            ? NSControlStateValueOn : NSControlStateValueOff;
        return box;
    }
    case T::Choice: {
        NSPopUpButton *popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        for (const std::string &choice : field.choices) [popup addItemWithTitle:@(choice.c_str())];
        // The current value, by value or (without values) by title.
        const std::vector<std::string> &values =
            field.choiceValues.size() == field.choices.size() ? field.choiceValues : field.choices;
        for (size_t i = 0; i < values.size(); ++i) {
            if (field.value.IsString() && field.value.GetString() == values[i]) {
                [popup selectItemAtIndex:NSInteger(i)];
            }
        }
        return popup;
    }
    default: {
        NSTextField *text = [NSTextField textFieldWithString:Text(field.value)];
        text.translatesAutoresizingMaskIntoConstraints = NO;
        [text.widthAnchor constraintGreaterThanOrEqualToConstant:
            field.type == T::String ? 240 : 120].active = YES;
        return text;
    }
    }
}

JsValue Value(const vela::FormField &field, NSView *control) {
    using T = vela::FormField::Type;
    switch (field.type) {
    case T::Bool:
        return JsValue(((NSButton *)control).state == NSControlStateValueOn);
    case T::Choice: {
        const NSInteger index = ((NSPopUpButton *)control).indexOfSelectedItem;
        const std::vector<std::string> &values =
            field.choiceValues.size() == field.choices.size() ? field.choiceValues : field.choices;
        return index >= 0 && size_t(index) < values.size() ? JsValue(values[size_t(index)])
                                                            : JsValue(std::string());
    }
    case T::String:
        return JsValue(std::string(((NSTextField *)control).stringValue.UTF8String ?: ""));
    case T::Int:
    case T::Float: {
        NSScanner *scanner = [NSScanner scannerWithString:
            [((NSTextField *)control).stringValue stringByTrimmingCharactersInSet:
                NSCharacterSet.whitespaceCharacterSet]];
        if (field.type == T::Int) {
            long long number;
            if ([scanner scanLongLong:&number] && scanner.isAtEnd) return JsValue(int64_t(number));
        } else {
            double number;
            if ([scanner scanDouble:&number] && scanner.isAtEnd) return JsValue(number);
        }
        return field.value;
    }
    }
    return field.value;
}

} // namespace

void VelaRunForm(NSWindow *window, NSString *title,
                 const std::vector<vela::FormField> &fields,
                 void (^completion)(const JsObject &values)) {
    NSGridView *grid = [NSGridView gridViewWithNumberOfColumns:2 rows:0];
    grid.rowSpacing = 8;
    grid.columnSpacing = 12;
    NSMutableArray<NSView *> *controls = [NSMutableArray array];
    for (const vela::FormField &field : fields) {
        NSTextField *label = [NSTextField labelWithString:@(field.label.c_str())];
        NSView *control = Control(field);
        [controls addObject:control];
        [grid addRowWithViews:@[label, control]];
    }
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    grid.frame = NSMakeRect(0, 0, grid.fittingSize.width, grid.fittingSize.height);

    NSAlert *alert = [NSAlert new];
    alert.messageText = title;
    alert.accessoryView = grid;
    [alert addButtonWithTitle:@"OK"];
    [alert addButtonWithTitle:@"Cancel"];
    std::vector<vela::FormField> copy = fields;
    void (^finish)(NSModalResponse) = ^(NSModalResponse response) {
        if (response != NSAlertFirstButtonReturn) return;
        JsObject values;
        for (size_t i = 0; i < copy.size(); ++i) {
            values[copy[i].key] = Value(copy[i], controls[i]);
        }
        completion(values);
    };
    if (window) {
        [alert beginSheetModalForWindow:window completionHandler:finish];
    } else {
        finish([alert runModal]);
    }
}
