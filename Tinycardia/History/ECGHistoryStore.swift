import Foundation

struct ECGScanRecord: Codable, Identifiable, Equatable {
    let id: UUID
    let capturedAt: Date
    let inferenceID: UInt32
    let wearableTimestampMilliseconds: UInt32
    let classificationRawValue: UInt8
    let qualityRawValue: UInt8
    let confidenceBasisPoints: UInt16?
    let deviceName: String
    let deviceIdentifier: String
    let sampleRateHz: Double
    let firstSampleTimestampMilliseconds: Double?
    let sampleValues: Data

    var classification: ECGClassification {
        ECGClassification(rawValue: classificationRawValue) ?? .unknown
    }

    var quality: SignalQuality {
        SignalQuality(rawValue: qualityRawValue) ?? .unknown
    }

    var confidencePercent: Double? {
        confidenceBasisPoints.map { Double($0) / 100.0 }
    }

    var sampleCount: Int { sampleValues.count / MemoryLayout<Int32>.size }

    var durationSeconds: Double {
        guard sampleRateHz > 0 else { return 0 }
        return Double(sampleCount) / sampleRateHz
    }

    var samples: [ECGSample] {
        let interval = 1_000.0 / sampleRateHz
        let start = firstSampleTimestampMilliseconds ?? 0
        return stride(from: 0, to: sampleValues.count, by: 4).enumerated().map { index, offset in
            let value = UInt32(sampleValues[offset])
                | (UInt32(sampleValues[offset + 1]) << 8)
                | (UInt32(sampleValues[offset + 2]) << 16)
                | (UInt32(sampleValues[offset + 3]) << 24)
            return ECGSample(
                id: UInt64(index),
                timestampMilliseconds: start + (Double(index) * interval),
                rawValue: Int32(bitPattern: value)
            )
        }
    }

    init(
        inference: InferenceResult,
        samples: [ECGSample],
        deviceName: String,
        deviceIdentifier: String,
        capturedAt: Date = .now
    ) {
        id = UUID()
        self.capturedAt = capturedAt
        inferenceID = inference.inferenceID
        wearableTimestampMilliseconds = inference.timestampMilliseconds
        classificationRawValue = inference.classification.rawValue
        qualityRawValue = inference.quality.rawValue
        confidenceBasisPoints = inference.confidenceBasisPoints
        self.deviceName = deviceName
        self.deviceIdentifier = deviceIdentifier
        sampleRateHz = TinycardiaProtocol.sampleRateHz
        firstSampleTimestampMilliseconds = samples.first?.timestampMilliseconds

        var encodedSamples = Data(capacity: samples.count * MemoryLayout<Int32>.size)
        for sample in samples {
            var value = sample.rawValue.littleEndian
            withUnsafeBytes(of: &value) { encodedSamples.append(contentsOf: $0) }
        }
        sampleValues = encodedSamples
    }
}

@MainActor
final class ECGHistoryStore: ObservableObject {
    static let retentionDays = 14

    @Published private(set) var records: [ECGScanRecord] = []
    @Published private(set) var persistenceMessage: String?

    private let fileURL: URL
    private let encoder: PropertyListEncoder
    private let decoder = PropertyListDecoder()

    init(fileManager: FileManager = .default) {
        let baseURL = fileManager.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? fileManager.temporaryDirectory
        let directoryURL = baseURL.appendingPathComponent("Tinycardia", isDirectory: true)
        fileURL = directoryURL.appendingPathComponent("ECGHistory.plist")

        encoder = PropertyListEncoder()
        encoder.outputFormat = .binary

        do {
            try fileManager.createDirectory(at: directoryURL, withIntermediateDirectories: true)
            if fileManager.fileExists(atPath: fileURL.path) {
                records = try decoder.decode([ECGScanRecord].self, from: Data(contentsOf: fileURL))
            }
            pruneExpiredRecords(now: .now, persistAfterPruning: true)
        } catch {
            records = []
            persistenceMessage = "History could not be loaded: \(error.localizedDescription)"
        }
    }

    func capture(
        inference: InferenceResult,
        samples: [ECGSample],
        deviceName: String?,
        deviceIdentifier: UUID?
    ) {
        let identifier = deviceIdentifier?.uuidString ?? "Unknown device"
        let now = Date.now

        let isRecentDuplicate = records.contains { record in
            record.inferenceID == inference.inferenceID
                && record.wearableTimestampMilliseconds == inference.timestampMilliseconds
                && record.deviceIdentifier == identifier
                && abs(record.capturedAt.timeIntervalSince(now)) < 60
        }
        guard !isRecentDuplicate else { return }

        records.insert(
            ECGScanRecord(
                inference: inference,
                samples: samples,
                deviceName: deviceName ?? "Tinycardia",
                deviceIdentifier: identifier,
                capturedAt: now
            ),
            at: 0
        )
        pruneExpiredRecords(now: now, persistAfterPruning: false)
        persist()
    }

    func delete(at offsets: IndexSet, from visibleRecords: [ECGScanRecord]) {
        let ids = Set(offsets.compactMap { index in
            visibleRecords.indices.contains(index) ? visibleRecords[index].id : nil
        })
        records.removeAll { ids.contains($0.id) }
        persist()
    }

    func pruneExpiredRecords(now: Date = .now, persistAfterPruning: Bool = true) {
        guard let cutoff = Calendar.current.date(byAdding: .day, value: -Self.retentionDays, to: now) else {
            return
        }
        let countBeforePruning = records.count
        records.removeAll { $0.capturedAt < cutoff }
        records.sort { $0.capturedAt > $1.capturedAt }
        if persistAfterPruning, records.count != countBeforePruning {
            persist()
        }
    }

    private func persist() {
        do {
            try encoder.encode(records).write(to: fileURL, options: .atomic)
            persistenceMessage = nil
        } catch {
            persistenceMessage = "History could not be saved: \(error.localizedDescription)"
        }
    }
}
