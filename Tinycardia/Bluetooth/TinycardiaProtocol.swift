import Foundation

enum TinycardiaProtocol {
    static let version: UInt8 = 0x01
    static let sampleRateHz = 256.0
    static let maximumECGSamplesPerPacket = 10

    static func decodeECGPacket(_ data: Data) throws -> ECGPacket {
        guard data.count >= 10 else {
            throw TinycardiaProtocolError.invalidLength(expected: "at least 10", actual: data.count)
        }
        try validateVersion(data[0])

        let sampleCount = Int(data[9])
        guard (1...maximumECGSamplesPerPacket).contains(sampleCount) else {
            throw TinycardiaProtocolError.invalidSampleCount(sampleCount)
        }
        let expectedLength = 10 + (sampleCount * 4)
        guard data.count == expectedLength else {
            throw TinycardiaProtocolError.invalidLength(expected: "\(expectedLength)", actual: data.count)
        }

        var samples = [Int32]()
        samples.reserveCapacity(sampleCount)
        for index in 0..<sampleCount {
            let rawValue = try data.uint32LittleEndian(at: 10 + (index * 4))
            samples.append(Int32(bitPattern: rawValue))
        }

        return ECGPacket(
            sequence: try data.uint32LittleEndian(at: 1),
            firstSampleTimestampMilliseconds: try data.uint32LittleEndian(at: 5),
            samples: samples
        )
    }

    static func decodeInferenceResult(_ data: Data) throws -> InferenceResult {
        guard data.count == 13 else {
            throw TinycardiaProtocolError.invalidLength(expected: "13", actual: data.count)
        }
        try validateVersion(data[0])

        guard let classification = ECGClassification(rawValue: data[9]) else {
            throw TinycardiaProtocolError.invalidEnum(field: "classification", rawValue: data[9])
        }
        guard let quality = SignalQuality(rawValue: data[10]) else {
            throw TinycardiaProtocolError.invalidEnum(field: "signal quality", rawValue: data[10])
        }
        let rawConfidence = try data.uint16LittleEndian(at: 11)
        guard rawConfidence == UInt16.max || rawConfidence <= 10_000 else {
            throw TinycardiaProtocolError.invalidConfidence(rawConfidence)
        }

        return InferenceResult(
            inferenceID: try data.uint32LittleEndian(at: 1),
            timestampMilliseconds: try data.uint32LittleEndian(at: 5),
            classification: classification,
            quality: quality,
            confidenceBasisPoints: rawConfidence == UInt16.max ? nil : rawConfidence
        )
    }

    static func decodeDeviceStatus(_ data: Data) throws -> DeviceStatus {
        guard data.count == 19 else {
            throw TinycardiaProtocolError.invalidLength(expected: "19", actual: data.count)
        }
        try validateVersion(data[0])

        guard let leadStatus = LeadStatus(rawValue: data[17]) else {
            throw TinycardiaProtocolError.invalidEnum(field: "lead status", rawValue: data[17])
        }
        guard let operatingState = OperatingState(rawValue: data[18]) else {
            throw TinycardiaProtocolError.invalidEnum(field: "operating state", rawValue: data[18])
        }

        return DeviceStatus(
            uptimeSeconds: try data.uint32LittleEndian(at: 1),
            samplesAcquired: try data.uint32LittleEndian(at: 5),
            samplesDropped: try data.uint32LittleEndian(at: 9),
            inferenceCount: try data.uint32LittleEndian(at: 13),
            leadStatus: leadStatus,
            operatingState: operatingState
        )
    }

    private static func validateVersion(_ receivedVersion: UInt8) throws {
        guard receivedVersion == version else {
            throw TinycardiaProtocolError.unsupportedVersion(receivedVersion)
        }
    }
}

enum TinycardiaControlCommand: UInt8 {
    case startStream = 0x01
    case stopStream = 0x02
    case startMonitoring = 0x03
    case stopMonitoring = 0x04
}

struct ECGPacket: Equatable {
    let sequence: UInt32
    let firstSampleTimestampMilliseconds: UInt32
    let samples: [Int32]
}

struct ECGSample: Identifiable, Equatable {
    let id: UInt64
    let timestampMilliseconds: Double
    let rawValue: Int32
}

enum ECGClassification: UInt8, Equatable {
    case normal = 0x00
    case atrialFibrillation = 0x01
    case unknown = 0x02

    var title: String {
        switch self {
        case .normal: "Normal"
        case .atrialFibrillation: "AFib"
        case .unknown: "Unknown"
        }
    }
}

enum SignalQuality: UInt8, Equatable {
    case good = 0x00
    case poor = 0x01
    case leadOff = 0x02
    case unknown = 0x03

    var title: String {
        switch self {
        case .good: "Good"
        case .poor: "Poor"
        case .leadOff: "Lead off"
        case .unknown: "Unknown"
        }
    }
}

struct InferenceResult: Identifiable, Equatable {
    let inferenceID: UInt32
    let timestampMilliseconds: UInt32
    let classification: ECGClassification
    let quality: SignalQuality
    let confidenceBasisPoints: UInt16?

    var id: UInt32 { inferenceID }
    var confidencePercent: Double? {
        confidenceBasisPoints.map { Double($0) / 100.0 }
    }
}

enum LeadStatus: UInt8, Equatable {
    case good = 0x00
    case lead1Off = 0x01
    case lead2Off = 0x02
    case bothOff = 0x03
    case checking = 0x04
    case unknown = 0x05

    var title: String {
        switch self {
        case .good: "Good"
        case .lead1Off: "Lead 1 off"
        case .lead2Off: "Lead 2 off"
        case .bothOff: "Both leads off"
        case .checking: "Checking"
        case .unknown: "Unknown"
        }
    }
}

enum OperatingState: UInt8, Equatable {
    case idle = 0x00
    case monitoring = 0x01
    case monitoringAndStreaming = 0x02
    case error = 0x03

    var title: String {
        switch self {
        case .idle: "Idle"
        case .monitoring: "Monitoring"
        case .monitoringAndStreaming: "Monitoring + streaming"
        case .error: "Error"
        }
    }
}

struct DeviceStatus: Equatable {
    let uptimeSeconds: UInt32
    let samplesAcquired: UInt32
    let samplesDropped: UInt32
    let inferenceCount: UInt32
    let leadStatus: LeadStatus
    let operatingState: OperatingState
}

enum TinycardiaProtocolError: LocalizedError {
    case invalidLength(expected: String, actual: Int)
    case unsupportedVersion(UInt8)
    case invalidSampleCount(Int)
    case invalidEnum(field: String, rawValue: UInt8)
    case invalidConfidence(UInt16)
    case truncatedField(offset: Int)

    var errorDescription: String? {
        switch self {
        case let .invalidLength(expected, actual):
            "Invalid packet length: expected \(expected) bytes, received \(actual)."
        case let .unsupportedVersion(version):
            "Unsupported Tinycardia protocol version \(version)."
        case let .invalidSampleCount(count):
            "Invalid ECG sample count \(count)."
        case let .invalidEnum(field, rawValue):
            "Invalid \(field) value \(rawValue)."
        case let .invalidConfidence(confidence):
            "Invalid confidence value \(confidence)."
        case let .truncatedField(offset):
            "Packet field at offset \(offset) is truncated."
        }
    }
}

private extension Data {
    func uint16LittleEndian(at offset: Int) throws -> UInt16 {
        guard offset >= 0, offset + 2 <= count else {
            throw TinycardiaProtocolError.truncatedField(offset: offset)
        }
        return UInt16(self[offset]) | (UInt16(self[offset + 1]) << 8)
    }

    func uint32LittleEndian(at offset: Int) throws -> UInt32 {
        guard offset >= 0, offset + 4 <= count else {
            throw TinycardiaProtocolError.truncatedField(offset: offset)
        }
        return UInt32(self[offset])
            | (UInt32(self[offset + 1]) << 8)
            | (UInt32(self[offset + 2]) << 16)
            | (UInt32(self[offset + 3]) << 24)
    }
}
