import Combine
import CoreBluetooth
import Foundation

final class BluetoothManager: NSObject, ObservableObject {
    @Published private(set) var bluetoothAvailability: BluetoothAvailability = .unknown
    @Published private(set) var isScanning = false
    @Published private(set) var connectionState: ConnectionState = .disconnected
    @Published private(set) var discoveredDevice: DiscoveredDevice?
    @Published private(set) var connectedDeviceName: String?
    @Published private(set) var gattDiscoveryState: GATTDiscoveryState = .notStarted
    @Published private(set) var gattVerification = GATTVerification()
    @Published private(set) var streamingState: StreamingState = .inactive
    @Published private(set) var ecgSamples = [ECGSample]()
    @Published private(set) var latestInference: InferenceResult?
    @Published private(set) var deviceStatus: DeviceStatus?
    @Published private(set) var receivedSampleCount = 0
    @Published private(set) var missingPacketCount = 0
    @Published private(set) var protocolErrorCount = 0
    @Published private(set) var statusMessage = "Checking Bluetooth availability…"

    private var centralManager: CBCentralManager!
    private var activePeripheral: CBPeripheral?
    private var pendingCharacteristicServices = Set<CBUUID>()
    private var pendingNotificationUUIDs = Set<CBUUID>()
    private var pendingControlCommand: TinycardiaControlCommand?

    private var waveformBuffer = [ECGSample]()
    private var waveformPublishWorkItem: DispatchWorkItem?
    private var nextSampleID: UInt64 = 0
    private var lastECGSequence: UInt32?
    private var rawReceivedSampleCount = 0
    private var rawMissingPacketCount = 0

    private(set) var ecgStreamCharacteristic: CBCharacteristic?
    private(set) var inferenceResultCharacteristic: CBCharacteristic?
    private(set) var deviceStatusCharacteristic: CBCharacteristic?
    private(set) var deviceControlCharacteristic: CBCharacteristic?
    private(set) var batteryLevelCharacteristic: CBCharacteristic?

    override init() {
        super.init()
        centralManager = CBCentralManager(delegate: self, queue: .main)
    }

    func startScanning() {
        guard centralManager.state == .poweredOn else {
            statusMessage = "Bluetooth must be available before scanning."
            return
        }
        guard connectionState != .connecting, activePeripheral == nil else { return }

        if centralManager.isScanning {
            centralManager.stopScan()
        }
        discoveredDevice = nil
        centralManager.scanForPeripherals(
            withServices: TinycardiaUUIDs.advertisedServices,
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: false]
        )
        isScanning = true
        statusMessage = "Scanning for a Tinycardia wearable…"
        log("Scan started for service \(TinycardiaUUIDs.service.uuidString)")
    }

    func stopScanning() {
        guard centralManager.isScanning || isScanning else { return }
        centralManager.stopScan()
        isScanning = false
        statusMessage = discoveredDevice == nil ? "Scan stopped." : "Tinycardia found. Ready to connect."
        log("Scan stopped")
    }

    func connect(to device: DiscoveredDevice) {
        guard centralManager.state == .poweredOn else {
            statusMessage = "Bluetooth is not available."
            return
        }
        guard connectionState != .connecting else { return }

        stopScanning()
        clearGATTState()
        activePeripheral = device.peripheral
        connectionState = .connecting
        statusMessage = "Connecting to \(device.name)…"
        log("Connection attempt: \(device.name) [\(device.id.uuidString)]")
        centralManager.connect(device.peripheral, options: nil)
    }

    func disconnect() {
        guard let peripheral = activePeripheral else { return }
        statusMessage = "Disconnecting from \(peripheral.name ?? "Tinycardia")…"
        centralManager.cancelPeripheralConnection(peripheral)
    }

    func startLiveStreaming() {
        guard gattVerification.isLiveDataReady else {
            failLiveSession("Required live-data characteristics are unavailable.")
            return
        }
        guard let peripheral = activePeripheral,
              let ecgStreamCharacteristic,
              let inferenceResultCharacteristic else {
            failLiveSession("The connected peripheral is unavailable.")
            return
        }
        guard streamingState != .subscribing,
              streamingState != .starting,
              streamingState != .live,
              streamingState != .stopping else { return }

        pendingControlCommand = nil
        pendingNotificationUUIDs.removeAll()
        streamingState = .subscribing
        statusMessage = "Subscribing to live ECG and inference results…"

        var notificationCharacteristics = [ecgStreamCharacteristic, inferenceResultCharacteristic]
        if gattVerification.deviceStatus == .found, let deviceStatusCharacteristic {
            notificationCharacteristics.append(deviceStatusCharacteristic)
            peripheral.readValue(for: deviceStatusCharacteristic)
        }

        for characteristic in notificationCharacteristics where !characteristic.isNotifying {
            pendingNotificationUUIDs.insert(characteristic.uuid)
            peripheral.setNotifyValue(true, for: characteristic)
        }

        log("Live subscriptions requested: \(notificationCharacteristics.map(\.uuid.uuidString).joined(separator: ", "))")
        if pendingNotificationUUIDs.isEmpty {
            sendControl(.startMonitoring)
        }
    }

    func stopLiveStreaming() {
        guard streamingState == .live else { return }
        sendControl(.stopStream)
    }

    private func sendControl(_ command: TinycardiaControlCommand) {
        guard let peripheral = activePeripheral, let deviceControlCharacteristic else {
            failLiveSession("Device Control is unavailable.")
            return
        }
        guard pendingControlCommand == nil else { return }

        pendingControlCommand = command
        streamingState = command == .stopStream ? .stopping : .starting
        statusMessage = controlStatusMessage(for: command)
        peripheral.writeValue(
            Data([command.rawValue]),
            for: deviceControlCharacteristic,
            type: .withResponse
        )
        log("Control write requested: \(command)")
    }

    private func controlStatusMessage(for command: TinycardiaControlCommand) -> String {
        switch command {
        case .startMonitoring: "Ensuring ECG monitoring is active…"
        case .startStream: "Starting the live ECG stream…"
        case .stopStream: "Pausing the live ECG stream…"
        case .stopMonitoring: "Stopping ECG monitoring…"
        }
    }

    private func clearConnection() {
        activePeripheral?.delegate = nil
        activePeripheral = nil
        connectedDeviceName = nil
        pendingCharacteristicServices.removeAll()
        clearGATTState()
    }

    private func clearGATTState() {
        gattDiscoveryState = .notStarted
        gattVerification = GATTVerification()
        ecgStreamCharacteristic = nil
        inferenceResultCharacteristic = nil
        deviceStatusCharacteristic = nil
        deviceControlCharacteristic = nil
        batteryLevelCharacteristic = nil
        clearLiveState()
    }

    private func clearLiveState() {
        waveformPublishWorkItem?.cancel()
        waveformPublishWorkItem = nil
        pendingNotificationUUIDs.removeAll()
        pendingControlCommand = nil
        waveformBuffer.removeAll(keepingCapacity: true)
        ecgSamples = []
        latestInference = nil
        deviceStatus = nil
        streamingState = .inactive
        nextSampleID = 0
        lastECGSequence = nil
        rawReceivedSampleCount = 0
        rawMissingPacketCount = 0
        receivedSampleCount = 0
        missingPacketCount = 0
        protocolErrorCount = 0
    }

    private func markUnavailable(message: String) {
        if centralManager.isScanning {
            centralManager.stopScan()
        }
        isScanning = false
        discoveredDevice = nil
        if let peripheral = activePeripheral {
            centralManager.cancelPeripheralConnection(peripheral)
        }
        clearConnection()
        connectionState = .disconnected
        statusMessage = message
    }

    private func finishCharacteristicDiscovery(for service: CBService) {
        pendingCharacteristicServices.remove(service.uuid)
        guard pendingCharacteristicServices.isEmpty else { return }

        if gattVerification.isComplete {
            gattDiscoveryState = .complete
            statusMessage = "GATT interface verified. Preparing live data…"
            log("GATT verification complete")
        } else if gattVerification.isTerminal {
            gattDiscoveryState = .incomplete
            statusMessage = "One or more GATT items are missing or have incompatible properties."
            log("GATT verification incomplete")
        }

        if gattVerification.isLiveDataReady {
            startLiveStreaming()
        }
    }

    private func markCharacteristicsMissing(for serviceUUID: CBUUID) {
        if serviceUUID == TinycardiaUUIDs.service {
            gattVerification.ecgStream = .missing
            gattVerification.inferenceResult = .missing
            gattVerification.deviceStatus = .missing
            gattVerification.deviceControl = .missing
        } else if serviceUUID == TinycardiaUUIDs.batteryService {
            gattVerification.batteryLevel = .missing
        }
    }

    private func verificationState(
        for characteristic: CBCharacteristic?,
        requiring requiredProperties: CBCharacteristicProperties
    ) -> DiscoveryItemState {
        guard let characteristic else { return .missing }
        return characteristic.properties.contains(requiredProperties) ? .found : .invalidProperties
    }

    private func appendECGPacket(_ packet: ECGPacket) {
        if let lastSequence = lastECGSequence {
            let sequenceDelta = packet.sequence &- lastSequence
            if sequenceDelta > 1, sequenceDelta < 10_000 {
                rawMissingPacketCount += Int(sequenceDelta - 1)
            } else if sequenceDelta >= 10_000 {
                log("ECG sequence reset detected: \(lastSequence) -> \(packet.sequence)")
            }
        }
        lastECGSequence = packet.sequence

        let sampleIntervalMilliseconds = 1_000.0 / TinycardiaProtocol.sampleRateHz
        for (index, rawValue) in packet.samples.enumerated() {
            waveformBuffer.append(
                ECGSample(
                    id: nextSampleID,
                    timestampMilliseconds: Double(packet.firstSampleTimestampMilliseconds)
                        + (Double(index) * sampleIntervalMilliseconds),
                    rawValue: rawValue
                )
            )
            nextSampleID &+= 1
        }
        rawReceivedSampleCount += packet.samples.count

        let maximumVisibleSamples = Int(TinycardiaProtocol.sampleRateHz * 10.0)
        if waveformBuffer.count > maximumVisibleSamples {
            waveformBuffer.removeFirst(waveformBuffer.count - maximumVisibleSamples)
        }
        scheduleWaveformPublish()
    }

    private func scheduleWaveformPublish() {
        guard waveformPublishWorkItem == nil else { return }

        let workItem = DispatchWorkItem { [weak self] in
            guard let self else { return }
            self.ecgSamples = self.waveformBuffer
            self.receivedSampleCount = self.rawReceivedSampleCount
            self.missingPacketCount = self.rawMissingPacketCount
            self.waveformPublishWorkItem = nil
        }
        waveformPublishWorkItem = workItem
        DispatchQueue.main.asyncAfter(deadline: .now() + (1.0 / 30.0), execute: workItem)
    }

    private func handleProtocolError(_ error: Error, characteristic: CBCharacteristic) {
        protocolErrorCount += 1
        statusMessage = "Invalid \(characteristic.uuid.uuidString) packet: \(error.localizedDescription)"
        log("Protocol decode failed for \(characteristic.uuid.uuidString): \(error.localizedDescription)")
    }

    private func failLiveSession(_ detail: String) {
        pendingControlCommand = nil
        pendingNotificationUUIDs.removeAll()
        streamingState = .failed(detail)
        statusMessage = "Live session failed: \(detail)"
        log("Live session failed: \(detail)")
    }

    private func log(_ message: String) {
        print("[Bluetooth] \(message)")
    }
}

extension BluetoothManager: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        bluetoothAvailability = BluetoothAvailability(central.state)
        log("State changed: \(bluetoothAvailability.title)")

        switch central.state {
        case .poweredOn:
            statusMessage = "Bluetooth is available."
            if activePeripheral == nil, connectionState != .connecting {
                startScanning()
            }
        case .poweredOff:
            markUnavailable(message: "Bluetooth is off. Turn it on to find Tinycardia.")
        case .unauthorized:
            markUnavailable(message: "Bluetooth access is not authorized. Enable it in Settings.")
        case .unsupported:
            markUnavailable(message: "Bluetooth Low Energy is not supported on this device.")
        case .resetting:
            markUnavailable(message: "Bluetooth is resetting. Please wait.")
        case .unknown:
            markUnavailable(message: "Bluetooth availability is unknown.")
        @unknown default:
            markUnavailable(message: "Bluetooth is unavailable.")
        }
    }

    func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        let advertisedName = advertisementData[CBAdvertisementDataLocalNameKey] as? String
        discoveredDevice = DiscoveredDevice(
            peripheral: peripheral,
            advertisedName: advertisedName,
            rssi: RSSI.intValue
        )
        log("Discovered \(discoveredDevice?.name ?? "Tinycardia") [\(peripheral.identifier.uuidString)]")
        stopScanning()
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        activePeripheral = peripheral
        connectedDeviceName = peripheral.name ?? discoveredDevice?.name ?? "Tinycardia"
        connectionState = .connected
        peripheral.delegate = self
        gattDiscoveryState = .discoveringServices
        statusMessage = "Connected. Discovering services…"
        log("Connected to \(connectedDeviceName ?? "Tinycardia")")
        peripheral.discoverServices(TinycardiaUUIDs.services)
    }

    func centralManager(
        _ central: CBCentralManager,
        didFailToConnect peripheral: CBPeripheral,
        error: Error?
    ) {
        let detail = error?.localizedDescription ?? "Unknown connection error"
        clearConnection()
        connectionState = .failed(detail)
        statusMessage = "Connection failed: \(detail)"
        log("Failed to connect: \(detail)")
    }

    func centralManager(
        _ central: CBCentralManager,
        didDisconnectPeripheral peripheral: CBPeripheral,
        error: Error?
    ) {
        let device = DiscoveredDevice(peripheral: peripheral, advertisedName: peripheral.name, rssi: 0)
        clearConnection()
        discoveredDevice = central.state == .poweredOn ? device : nil
        connectionState = .disconnected

        if let error {
            statusMessage = "Disconnected unexpectedly: \(error.localizedDescription)"
            log("Disconnected with error: \(error.localizedDescription)")
        } else {
            statusMessage = "Disconnected. You can reconnect or scan again."
            log("Disconnected")
        }
    }
}

extension BluetoothManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error {
            gattVerification.tinycardiaService = .missing
            gattVerification.batteryService = .missing
            markCharacteristicsMissing(for: TinycardiaUUIDs.service)
            markCharacteristicsMissing(for: TinycardiaUUIDs.batteryService)
            gattDiscoveryState = .failed(error.localizedDescription)
            statusMessage = "Service discovery failed: \(error.localizedDescription)"
            log("Service discovery failed: \(error.localizedDescription)")
            return
        }

        let services = peripheral.services ?? []
        let tinycardiaService = services.first { $0.uuid == TinycardiaUUIDs.service }
        let batteryService = services.first { $0.uuid == TinycardiaUUIDs.batteryService }

        gattVerification.tinycardiaService = tinycardiaService == nil ? .missing : .found
        gattVerification.batteryService = batteryService == nil ? .missing : .found
        log("Services discovered: \(services.map(\.uuid.uuidString).joined(separator: ", "))")

        if let tinycardiaService {
            gattVerification.ecgStream = .pending
            gattVerification.inferenceResult = .pending
            gattVerification.deviceStatus = .pending
            gattVerification.deviceControl = .pending
            pendingCharacteristicServices.insert(tinycardiaService.uuid)
            peripheral.discoverCharacteristics(TinycardiaUUIDs.tinycardiaCharacteristics, for: tinycardiaService)
        } else {
            markCharacteristicsMissing(for: TinycardiaUUIDs.service)
        }

        if let batteryService {
            gattVerification.batteryLevel = .pending
            pendingCharacteristicServices.insert(batteryService.uuid)
            peripheral.discoverCharacteristics(TinycardiaUUIDs.batteryCharacteristics, for: batteryService)
        } else {
            markCharacteristicsMissing(for: TinycardiaUUIDs.batteryService)
        }

        if pendingCharacteristicServices.isEmpty {
            gattDiscoveryState = .incomplete
            statusMessage = "Connected, but the expected services are missing."
        } else {
            gattDiscoveryState = .discoveringCharacteristics
            statusMessage = "Services found. Discovering characteristics…"
        }
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didDiscoverCharacteristicsFor service: CBService,
        error: Error?
    ) {
        if let error {
            markCharacteristicsMissing(for: service.uuid)
            statusMessage = "Characteristic discovery issue: \(error.localizedDescription)"
            log("Characteristic discovery failed for \(service.uuid.uuidString): \(error.localizedDescription)")
            finishCharacteristicDiscovery(for: service)
            return
        }

        let characteristics = service.characteristics ?? []
        let characteristicSummary = characteristics.map {
            "\($0.uuid.uuidString) [properties=0x\(String($0.properties.rawValue, radix: 16))]"
        }
        log("Characteristics for \(service.uuid.uuidString): \(characteristicSummary.joined(separator: ", "))")

        if service.uuid == TinycardiaUUIDs.service {
            ecgStreamCharacteristic = characteristics.first { $0.uuid == TinycardiaUUIDs.ecgStream }
            inferenceResultCharacteristic = characteristics.first { $0.uuid == TinycardiaUUIDs.inferenceResult }
            deviceStatusCharacteristic = characteristics.first { $0.uuid == TinycardiaUUIDs.deviceStatus }
            deviceControlCharacteristic = characteristics.first { $0.uuid == TinycardiaUUIDs.deviceControl }

            gattVerification.ecgStream = verificationState(for: ecgStreamCharacteristic, requiring: .notify)
            gattVerification.inferenceResult = verificationState(
                for: inferenceResultCharacteristic,
                requiring: .notify
            )
            gattVerification.deviceStatus = verificationState(
                for: deviceStatusCharacteristic,
                requiring: [.read, .notify]
            )
            gattVerification.deviceControl = verificationState(for: deviceControlCharacteristic, requiring: .write)
        } else if service.uuid == TinycardiaUUIDs.batteryService {
            batteryLevelCharacteristic = characteristics.first { $0.uuid == TinycardiaUUIDs.batteryLevel }
            gattVerification.batteryLevel = verificationState(
                for: batteryLevelCharacteristic,
                requiring: [.read, .notify]
            )
        }

        finishCharacteristicDiscovery(for: service)
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateNotificationStateFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        guard pendingNotificationUUIDs.contains(characteristic.uuid) else { return }

        if let error {
            failLiveSession("Could not subscribe to \(characteristic.uuid.uuidString): \(error.localizedDescription)")
            return
        }
        guard characteristic.isNotifying else {
            failLiveSession("The wearable rejected \(characteristic.uuid.uuidString) notifications.")
            return
        }

        pendingNotificationUUIDs.remove(characteristic.uuid)
        log("Notifications active: \(characteristic.uuid.uuidString)")
        if pendingNotificationUUIDs.isEmpty, streamingState == .subscribing {
            sendControl(.startMonitoring)
        }
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didWriteValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        guard characteristic.uuid == TinycardiaUUIDs.deviceControl,
              let command = pendingControlCommand else { return }
        pendingControlCommand = nil

        if let error {
            failLiveSession("The wearable rejected \(command): \(error.localizedDescription)")
            return
        }

        log("Control write acknowledged: \(command)")
        switch command {
        case .startMonitoring:
            sendControl(.startStream)
        case .startStream:
            streamingState = .live
            statusMessage = "Live ECG and inference monitoring are active."
        case .stopStream:
            streamingState = .inactive
            statusMessage = "Live ECG paused. Inference monitoring remains active."
        case .stopMonitoring:
            streamingState = .inactive
            statusMessage = "ECG monitoring stopped."
        }
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        if let error {
            statusMessage = "BLE update failed: \(error.localizedDescription)"
            log("Value update failed for \(characteristic.uuid.uuidString): \(error.localizedDescription)")
            return
        }
        guard let data = characteristic.value else { return }

        do {
            switch characteristic.uuid {
            case TinycardiaUUIDs.ecgStream:
                appendECGPacket(try TinycardiaProtocol.decodeECGPacket(data))
            case TinycardiaUUIDs.inferenceResult:
                let result = try TinycardiaProtocol.decodeInferenceResult(data)
                latestInference = result
                log(
                    "Inference \(result.inferenceID): \(result.classification.title), "
                        + "confidence \(result.confidencePercent.map { String(format: "%.2f%%", $0) } ?? "unavailable")"
                )
            case TinycardiaUUIDs.deviceStatus:
                deviceStatus = try TinycardiaProtocol.decodeDeviceStatus(data)
            default:
                break
            }
        } catch {
            handleProtocolError(error, characteristic: characteristic)
        }
    }
}
