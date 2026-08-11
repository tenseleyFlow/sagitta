const Allocator = struct {
    ptr: *anyopaque,
    pub fn create(self: *@This(), comptime T: type) !*T {
        _ = self;
        return error.OutOfMemory;
    }
};
