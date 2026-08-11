protocol Service {
    associatedtype Output
    func fetch() async throws -> Output
}
struct App: Service {
    typealias Output = [String]
    func fetch() async throws -> Output { return ["ok"] }
}
