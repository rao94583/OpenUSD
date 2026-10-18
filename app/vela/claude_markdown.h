// Claude's replies are Markdown: headings, paragraphs, lists, code blocks,
// tables, emphasis, inline code and links, rendered as attributed text for
// the Claude panel. Foundation parses; this lays the blocks out.
#import <AppKit/AppKit.h>

NSAttributedString *VelaRenderMarkdown(NSString *markdown, CGFloat fontSize);
