#define VERSION 3
@protocol Worker
@required
- (id)run:(SEL)selector;
@optional
- (void)stop;
@end
