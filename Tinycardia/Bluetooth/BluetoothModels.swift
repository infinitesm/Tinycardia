import CoreBluetooth
import Foundation

enum BluetoothAvailability: Equatable {
    case unknown
    case available
    case off
    case unauthorized
    case unsupported

    init(_ state: CBManagerState) {
        switch state {
        case .poweredOn: self = .available
        case .poweredOff: self = .off
        case .unauthorized: self = .unauthorized
        case .unsupported: self = .unsupported
        case .unknown, .resetting: self = .unknown
        @unknown default: self = .unknown
        }
    }

    var title: String {
        switch self {
        case .unknown: "Unknown"
        case .available: "Available"
        case .off: "Off"
        case .unauthorized: "Unauthorized"
        case .unsupported: "Unsupported"
        }
    }
}

enum ConnectionState: Equatable {
    case disconnected
    case connecting
    case connected
    case failed(String)

    var title: String {
        switch self {
        case .disconnected: "Disconnected"
        case .connecting: "Connecting"
        case .connected: "Connected"
        case .failed: "Failed"
        }
    }
}

enum GATTDiscoveryState: Equatable {
    case notStarted
    case discoveringServices
    case discoveringCharacteristics
    case complete
    case incomplete
    case failed(String)

    var title: String {
        switch self {
        case .notStarted: "Not started"
        case .discoveringServices: "Discovering services"
        case .discoveringCharacteristics: "Discovering characteristics"
        case .complete: "Verified"
        case .incomplete: "Incomplete"
        case .failed: "Failed"
        }
    }
}

enum StreamingState: Equatable {
    case inactive
    case subscribing
    case starting
    case live
    case stopping
    case failed(String)

    var title: String {
        switch self {
        case .inactive: "Paused"
        case .subscribing: "Subscribing"
        case .starting: "Starting"
        case .live: "Live"
        case .stopping: "Stopping"
        case .failed: "Failed"
        }
    }
}

enum DiscoveryItemState: Equatable {
    case notChecked
    case pending
    case found
    case missing
    case invalidProperties

    var title: String {
        switch self {
        case .notChecked: "Not checked"
        case .pending: "Checking"
        case .found: "Found"
        case .missing: "Missing"
        case .invalidProperties: "Property mismatch"
        }
    }
}

struct GATTVerification: Equatable {
    var tinycardiaService: DiscoveryItemState = .notChecked
    var ecgStream: DiscoveryItemState = .notChecked
    var inferenceResult: DiscoveryItemState = .notChecked
    var deviceStatus: DiscoveryItemState = .notChecked
    var deviceControl: DiscoveryItemState = .notChecked
    var batteryService: DiscoveryItemState = .notChecked
    var batteryLevel: DiscoveryItemState = .notChecked

    var characteristicStates: [DiscoveryItemState] {
        [ecgStream, inferenceResult, deviceStatus, deviceControl, batteryLevel]
    }

    var isComplete: Bool {
        tinycardiaService == .found
            && batteryService == .found
            && characteristicStates.allSatisfy { $0 == .found }
    }

    var isLiveDataReady: Bool {
        tinycardiaService == .found
            && ecgStream == .found
            && inferenceResult == .found
            && deviceControl == .found
    }

    var isTerminal: Bool {
        characteristicStates.allSatisfy {
            $0 == .found || $0 == .missing || $0 == .invalidProperties
        }
    }
}

struct DiscoveredDevice: Identifiable {
    let peripheral: CBPeripheral
    let advertisedName: String?
    let rssi: Int

    var id: UUID { peripheral.identifier }
    var name: String { peripheral.name ?? advertisedName ?? "Tinycardia" }
}
