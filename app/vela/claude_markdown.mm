#import "claude_markdown.h"

#include <algorithm>

namespace {

NSFont *Monospaced(CGFloat size) {
    return [NSFont monospacedSystemFontOfSize:size weight:NSFontWeightRegular];
}

// The block a run belongs to, from its presentation intent (innermost
// first): the kind that decides its layout, list nesting and numbering.
struct Block {
    NSInteger identity = -1;      // the innermost intent's, to tell blocks apart
    NSInteger header = 0;         // heading level
    BOOL code = NO;
    BOOL quote = NO;
    NSInteger listDepth = 0;
    NSInteger itemIdentity = -1;  // the list item it starts, if any
    NSInteger ordinal = 0;        // its number in an ordered list, else 0
    NSInteger rowIdentity = -1;   // a table row
    NSInteger cellIdentity = -1;
    BOOL headerRow = NO;
};

Block Describe(NSPresentationIntent *intent) {
    Block block;
    if (!intent) return block;
    block.identity = intent.identity;
    BOOL ordered = NO;
    for (NSPresentationIntent *i = intent; i; i = i.parentIntent) {
        switch (i.intentKind) {
            case NSPresentationIntentKindHeader:
                block.header = i.headerLevel;
                break;
            case NSPresentationIntentKindCodeBlock:
                block.code = YES;
                break;
            case NSPresentationIntentKindBlockQuote:
                block.quote = YES;
                break;
            case NSPresentationIntentKindListItem:
                if (block.itemIdentity < 0) {
                    block.itemIdentity = i.identity;
                    block.ordinal = i.ordinal;
                    ordered = i.parentIntent.intentKind == NSPresentationIntentKindOrderedList;
                }
                break;
            case NSPresentationIntentKindOrderedList:
            case NSPresentationIntentKindUnorderedList:
                ++block.listDepth;
                break;
            case NSPresentationIntentKindTableRow:
                block.rowIdentity = i.identity;
                break;
            case NSPresentationIntentKindTableHeaderRow:
                block.rowIdentity = i.identity;
                block.headerRow = YES;
                break;
            case NSPresentationIntentKindTableCell:
                block.cellIdentity = i.identity;
                break;
            default:
                break;
        }
    }
    if (!ordered) block.ordinal = 0;
    return block;
}

NSParagraphStyle *Paragraph(const Block &block, CGFloat fontSize) {
    NSMutableParagraphStyle *style = [NSMutableParagraphStyle new];
    style.paragraphSpacing = block.code ? 0 : fontSize * 0.5;
    if (block.listDepth) {
        const CGFloat indent = 18.0 * CGFloat(block.listDepth);
        style.headIndent = indent;
        style.firstLineHeadIndent = indent - 14.0;
        style.tabStops = @[[[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft
                                                           location:indent options:@{}]];
        style.paragraphSpacing = fontSize * 0.25;
    }
    if (block.quote) {
        style.headIndent += 12;
        style.firstLineHeadIndent += 12;
    }
    if (block.code) {
        style.headIndent += 8;
        style.firstLineHeadIndent += 8;
    }
    if (block.rowIdentity >= 0) {
        NSMutableArray<NSTextTab *> *tabs = [NSMutableArray array];
        for (int i = 1; i < 8; ++i) {
            [tabs addObject:[[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft
                                                           location:150.0 * i options:@{}]];
        }
        style.tabStops = tabs;
        style.paragraphSpacing = 2;
    }
    if (block.header) {
        style.paragraphSpacingBefore = fontSize * 0.4;
        style.paragraphSpacing = fontSize * 0.3;
    }
    return style;
}

} // namespace

NSAttributedString *VelaRenderMarkdown(NSString *markdown, CGFloat fontSize) {
    NSFont *body = [NSFont systemFontOfSize:fontSize];
    NSDictionary *plain = @{NSFontAttributeName: body,
                            NSForegroundColorAttributeName: NSColor.textColor};
    NSAttributedStringMarkdownParsingOptions *options = [NSAttributedStringMarkdownParsingOptions new];
    options.interpretedSyntax = NSAttributedStringMarkdownInterpretedSyntaxFull;
    options.failurePolicy = NSAttributedStringMarkdownParsingFailureReturnPartiallyParsedIfPossible;
    NSError *error = nil;
    NSAttributedString *parsed = [[NSAttributedString alloc] initWithMarkdownString:markdown
                                                                            options:options
                                                                            baseURL:nil
                                                                              error:&error];
    if (!parsed) return [[NSAttributedString alloc] initWithString:markdown attributes:plain];

    NSMutableAttributedString *out = [NSMutableAttributedString new];
    __block Block previous;
    __block BOOL first = YES;
    NSString *source = parsed.string;
    [parsed enumerateAttributesInRange:NSMakeRange(0, parsed.length) options:0
                            usingBlock:^(NSDictionary<NSAttributedStringKey, id> *attributes,
                                         NSRange range, BOOL *) {
        const Block block = Describe(attributes[NSPresentationIntentAttributeName]);
        NSString *text = [source substringWithRange:range];
        NSMutableDictionary *style = [plain mutableCopy];
        style[NSParagraphStyleAttributeName] = Paragraph(block, fontSize);

        // Between blocks: a new line, and a list item's marker or a table
        // cell's tab.
        if (!first && block.identity != previous.identity) {
            const BOOL sameRow = block.rowIdentity >= 0 && block.rowIdentity == previous.rowIdentity;
            if (sameRow && block.cellIdentity != previous.cellIdentity) {
                [out appendAttributedString:[[NSAttributedString alloc] initWithString:@"\t"
                                                                            attributes:style]];
            } else if (!sameRow && ![out.string hasSuffix:@"\n"]) {
                [out appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n"
                                                                            attributes:style]];
            }
        }
        if (block.itemIdentity >= 0 && (first || block.itemIdentity != previous.itemIdentity)) {
            NSString *marker = block.ordinal
                ? [NSString stringWithFormat:@"%ld.\t", (long)block.ordinal]
                : (block.listDepth > 1 ? @"◦\t" : @"•\t");
            NSMutableDictionary *markerStyle = [style mutableCopy];
            markerStyle[NSForegroundColorAttributeName] = NSColor.secondaryLabelColor;
            [out appendAttributedString:[[NSAttributedString alloc] initWithString:marker
                                                                        attributes:markerStyle]];
        }
        first = NO;
        previous = block;

        // The run's own look.
        NSFont *font = body;
        if (block.header) {
            const CGFloat sizes[] = {1.35, 1.2, 1.1, 1.0, 1.0, 1.0};
            font = [NSFont boldSystemFontOfSize:fontSize * sizes[std::min<NSInteger>(block.header, 6) - 1]];
        }
        if (block.headerRow) font = [NSFont boldSystemFontOfSize:fontSize];
        const NSInlinePresentationIntent inlineIntent =
            NSInlinePresentationIntent([attributes[NSInlinePresentationIntentAttributeName]
                                        unsignedIntegerValue]);
        if (block.code || (inlineIntent & NSInlinePresentationIntentCode)) {
            font = Monospaced(fontSize * 0.92);
            style[NSBackgroundColorAttributeName] =
                [NSColor.labelColor colorWithAlphaComponent:0.06];
        }
        NSFontTraitMask traits = 0;
        if (inlineIntent & NSInlinePresentationIntentStronglyEmphasized) traits |= NSBoldFontMask;
        if (inlineIntent & NSInlinePresentationIntentEmphasized) traits |= NSItalicFontMask;
        if (traits) font = [NSFontManager.sharedFontManager convertFont:font toHaveTrait:traits];
        style[NSFontAttributeName] = font;
        if (inlineIntent & NSInlinePresentationIntentStrikethrough) {
            style[NSStrikethroughStyleAttributeName] = @(NSUnderlineStyleSingle);
        }
        if (block.quote) style[NSForegroundColorAttributeName] = NSColor.secondaryLabelColor;
        if (id link = attributes[NSLinkAttributeName]) style[NSLinkAttributeName] = link;
        // A code block's text ends with its newline; blocks add their own.
        if (block.code && [text hasSuffix:@"\n"]) text = [text substringToIndex:text.length - 1];
        [out appendAttributedString:[[NSAttributedString alloc] initWithString:text
                                                                    attributes:style]];
    }];
    return out;
}
