import CoreBluetooth

enum TinycardiaUUIDs {
    // Fixed UUID family for Tinycardia BLE protocol v1. These values mirror
    // firmware/nrf52840/include/ble_service.h.
    static let service = CBUUID(string: "F8A50001-7C5B-4E91-A6D2-3B1C9E4F5200")
    static let ecgStream = CBUUID(string: "F8A50002-7C5B-4E91-A6D2-3B1C9E4F5200")
    static let inferenceResult = CBUUID(string: "F8A50003-7C5B-4E91-A6D2-3B1C9E4F5200")
    static let deviceStatus = CBUUID(string: "F8A50004-7C5B-4E91-A6D2-3B1C9E4F5200")
    static let deviceControl = CBUUID(string: "F8A50005-7C5B-4E91-A6D2-3B1C9E4F5200")

    static let batteryService = CBUUID(string: "180F")
    static let batteryLevel = CBUUID(string: "2A19")

    static let advertisedServices = [service]
    static let services = [service, batteryService]
    static let tinycardiaCharacteristics = [
        ecgStream,
        inferenceResult,
        deviceStatus,
        deviceControl
    ]
    static let batteryCharacteristics = [batteryLevel]
}
