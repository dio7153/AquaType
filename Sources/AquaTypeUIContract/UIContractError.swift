import Foundation

/// What this library and the engine throw.
public enum UIContractError: Error, Equatable, LocalizedError {
    case invalidArgument(String)
    case invalidArtifact(String)
    case invalidUTF8(String)

    public var errorDescription: String? {
        switch self {
        case .invalidArgument(let message):
            return "invalid argument: \(message)"
        case .invalidArtifact(let message):
            return "invalid artifact: \(message)"
        case .invalidUTF8(let message):
            return "invalid UTF-8: \(message)"
        }
    }
}
