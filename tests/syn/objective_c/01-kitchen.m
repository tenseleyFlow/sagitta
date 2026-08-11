#import <Foundation/Foundation.h>
/// API docs
/* TODO block */
@interface Greeter : NSObject
@property(nonatomic, copy) NSString *name;
- (instancetype)initWithName:(NSString *)name;
@end
@implementation Greeter
- (void)say:(NSInteger)count {
  BOOL ok = @YES;
  NSNumber *n = @12.5;
  NSString *s = @"hello\\n";
  char c = '\\t'; const char *p = "c\\x41";
  printf("%d", count);
  if (ok && n != nil) [self emit:s count:count];
}
@end
*/
