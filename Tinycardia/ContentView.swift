import SwiftUI

struct ContentView: View {
    @ObservedObject var bluetoothManager: BluetoothManager
    @ObservedObject var historyStore: ECGHistoryStore
    @State private var selectedSection: AppSection = .bluetooth

    var body: some View {
        ZStack(alignment: .bottom) {
            TinycardiaTheme.background.ignoresSafeArea()

            Group {
                switch selectedSection {
                case .live:
                    livePage
                case .history:
                    ECGHistoryView(historyStore: historyStore)
                case .bluetooth:
                    bluetoothPage
                }
            }
            .id(selectedSection)
            .transition(.opacity)

            FloatingTabBar(selection: $selectedSection, isConnected: bluetoothManager.connectionState == .connected)
        }
        .animation(.easeInOut(duration: 0.22), value: selectedSection)
        .onChange(of: bluetoothManager.gattDiscoveryState) { _, state in
            if state == .complete {
                selectedSection = .live
            }
        }
    }

    private var livePage: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 16) {
                    liveStatusHeader
                    liveECGCard
                    inferenceCard

                    if bluetoothManager.deviceStatus != nil {
                        deviceStatusCard
                    }

                    if bluetoothManager.connectionState != .connected {
                        CardView(title: "Connect your wearable") {
                            Label(
                                "Open Bluetooth to find and connect Tinycardia before starting a live scan.",
                                systemImage: "wave.3.right"
                            )
                            .foregroundStyle(TinycardiaTheme.secondaryText)

                            Button("Open Bluetooth") {
                                selectedSection = .bluetooth
                            }
                            .buttonStyle(.borderedProminent)
                        }
                    }
                }
                .padding(.horizontal)
                .padding(.top, 8)
                .padding(.bottom, 112)
            }
            .navigationTitle("Live ECG")
        }
    }

    private var bluetoothPage: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 16) {
                    quickConnectCard
                    bluetoothCard
                    deviceCard

                    if bluetoothManager.connectionState == .connected
                        || bluetoothManager.gattDiscoveryState != .notStarted {
                        gattCard
                    }

                    statusCard
                }
                .padding(.horizontal)
                .padding(.top, 8)
                .padding(.bottom, 112)
            }
            .navigationTitle("Bluetooth")
        }
    }

    private var quickConnectCard: some View {
        CardView(title: quickConnectTitle) {
            HStack(alignment: .top, spacing: 13) {
                ZStack {
                    Circle()
                        .fill(connectionColor.opacity(0.14))
                        .frame(width: 48, height: 48)
                    Image(systemName: connectionSymbol)
                        .font(.title3.weight(.semibold))
                        .foregroundStyle(connectionColor)
                }

                VStack(alignment: .leading, spacing: 4) {
                    Text(quickConnectMessage)
                        .font(.subheadline)
                        .foregroundStyle(TinycardiaTheme.secondaryText)
                    if bluetoothManager.isScanning {
                        ProgressView()
                            .controlSize(.small)
                            .padding(.top, 3)
                    }
                }
            }

            Button {
                performPrimaryConnectionAction()
            } label: {
                Label(quickConnectButtonTitle, systemImage: quickConnectButtonSymbol)
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.large)
            .disabled(
                bluetoothManager.bluetoothAvailability != .available
                    || bluetoothManager.connectionState == .connecting
            )
        }
    }

    private var liveStatusHeader: some View {
        HStack(spacing: 12) {
            ZStack {
                Circle()
                    .fill(streamingColor.opacity(0.16))
                    .frame(width: 46, height: 46)
                Image(systemName: streamingSymbol)
                    .foregroundStyle(streamingColor)
                    .font(.title3.weight(.semibold))
            }

            VStack(alignment: .leading, spacing: 2) {
                Text(bluetoothManager.streamingState.title)
                    .font(.headline)
                Text(bluetoothManager.connectedDeviceName ?? "No wearable connected")
                    .font(.subheadline)
                    .foregroundStyle(TinycardiaTheme.secondaryText)
            }

            Spacer()
            streamingButton
        }
        .padding(14)
        .tinycardiaSurface(cornerRadius: 18)
    }

    private var bluetoothCard: some View {
        CardView(title: "Bluetooth") {
            statusRow(
                label: "State",
                value: bluetoothManager.bluetoothAvailability.title,
                symbol: bluetoothManager.bluetoothAvailability == .available
                    ? "checkmark.circle.fill" : "exclamationmark.circle.fill",
                color: bluetoothManager.bluetoothAvailability == .available
                    ? TinycardiaTheme.success : TinycardiaTheme.attention
            )

            Divider()

            HStack {
                Label(
                    bluetoothManager.isScanning ? "Scanning" : "Not scanning",
                    systemImage: bluetoothManager.isScanning
                        ? "antenna.radiowaves.left.and.right"
                        : "antenna.radiowaves.left.and.right.slash"
                )
                .foregroundStyle(TinycardiaTheme.secondaryText)
                Spacer()
                Button(bluetoothManager.isScanning ? "Stop" : "Scan") {
                    bluetoothManager.isScanning
                        ? bluetoothManager.stopScanning()
                        : bluetoothManager.startScanning()
                }
                .buttonStyle(.bordered)
                .disabled(
                    bluetoothManager.bluetoothAvailability != .available
                        || bluetoothManager.connectionState == .connecting
                        || bluetoothManager.connectionState == .connected
                )
            }
        }
    }

    private var deviceCard: some View {
        CardView(title: "Device") {
            if let device = bluetoothManager.discoveredDevice {
                VStack(alignment: .leading, spacing: 5) {
                    Text(device.name).font(.headline)
                    Text(device.id.uuidString)
                        .font(.caption.monospaced())
                        .foregroundStyle(TinycardiaTheme.secondaryText)
                    if device.rssi != 0 {
                        Text("Signal: \(device.rssi) dBm")
                            .font(.caption)
                            .foregroundStyle(TinycardiaTheme.secondaryText)
                    }
                }

                Divider()
                HStack {
                    statusRow(
                        label: "Connection",
                        value: bluetoothManager.connectionState.title,
                        symbol: connectionSymbol,
                        color: connectionColor
                    )
                    Spacer()
                    connectionButton(for: device)
                }
            } else {
                ContentUnavailableView(
                    bluetoothManager.isScanning ? "Looking for Tinycardia" : "No Tinycardia Found",
                    systemImage: "wave.3.right",
                    description: Text(
                        bluetoothManager.isScanning
                            ? "Keep the wearable nearby and powered on."
                            : "Start a scan when the wearable is advertising."
                    )
                )
                .frame(maxWidth: .infinity)
            }
        }
    }

    private var liveECGCard: some View {
        CardView(title: "Live ECG") {
            ZStack {
                ECGWaveformView(samples: bluetoothManager.ecgSamples)
                if bluetoothManager.ecgSamples.isEmpty {
                    VStack(spacing: 8) {
                        Image(systemName: "waveform.path.ecg").font(.title)
                        Text(livePlaceholderText).font(.subheadline)
                    }
                    .foregroundStyle(TinycardiaTheme.secondaryText)
                }
            }
            .frame(height: 230)

            HStack {
                metric("Rate", "256 Hz")
                Spacer()
                metric("Window", formattedWindowDuration)
                Spacer()
                metric("Missing packets", bluetoothManager.missingPacketCount.formatted())
            }

            Text("Auto-scaled raw MAX30003 values. Rolling 10-second window.")
                .font(.caption)
                .foregroundStyle(TinycardiaTheme.secondaryText)
        }
    }

    private var inferenceCard: some View {
        CardView(title: "Latest Inference") {
            if let inference = bluetoothManager.latestInference {
                HStack(alignment: .firstTextBaseline) {
                    Label(inference.classification.title, systemImage: inferenceSymbol(inference.classification))
                        .font(.title2.weight(.semibold))
                        .foregroundStyle(inferenceColor(inference.classification))
                    Spacer()
                    if let confidence = inference.confidencePercent {
                        Text(confidence, format: .number.precision(.fractionLength(1)))
                            .font(.title2.monospacedDigit().weight(.semibold))
                        Text("%").foregroundStyle(TinycardiaTheme.secondaryText)
                    } else {
                        Text("Confidence unavailable").foregroundStyle(TinycardiaTheme.secondaryText)
                    }
                }

                Divider()
                valueRow("Signal quality", inference.quality.title)
                valueRow("Inference ID", inference.inferenceID.formatted())
                valueRow("Wearable timestamp", "\(inference.timestampMilliseconds) ms")

                Label("Saved automatically with its ECG window", systemImage: "checkmark.circle.fill")
                    .font(.caption)
                    .foregroundStyle(TinycardiaTheme.success)
            } else {
                HStack(spacing: 12) {
                    if bluetoothManager.streamingState == .live {
                        ProgressView()
                    } else {
                        Image(systemName: "brain.head.profile").foregroundStyle(TinycardiaTheme.secondaryText)
                    }
                    VStack(alignment: .leading, spacing: 3) {
                        Text("Waiting for an inference result").font(.headline)
                        Text("The wearable needs a clean 10-second ECG window and good lead contact.")
                            .font(.caption)
                            .foregroundStyle(TinycardiaTheme.secondaryText)
                    }
                }
            }

            Text("Prototype model output. This is not a medical diagnosis.")
                .font(.caption)
                .foregroundStyle(TinycardiaTheme.secondaryText)
        }
    }

    private var deviceStatusCard: some View {
        CardView(title: "Wearable Status") {
            if let status = bluetoothManager.deviceStatus {
                valueRow("Operating state", status.operatingState.title)
                valueRow("Lead contact", status.leadStatus.title)
                valueRow("Samples acquired", status.samplesAcquired.formatted())
                valueRow("Samples dropped", status.samplesDropped.formatted())
                valueRow("Completed inferences", status.inferenceCount.formatted())
                valueRow("Uptime", formattedDuration(seconds: status.uptimeSeconds))
            }
        }
    }

    private var gattCard: some View {
        CardView(title: "GATT Verification") {
            statusRow(
                label: "Discovery",
                value: bluetoothManager.gattDiscoveryState.title,
                symbol: bluetoothManager.gattDiscoveryState == .complete
                    ? "checkmark.seal.fill" : "magnifyingglass",
                color: bluetoothManager.gattDiscoveryState == .complete
                    ? TinycardiaTheme.success : TinycardiaTheme.active
            )

            Divider()
            Text("Tinycardia Service").font(.headline)
            discoveryRow("Service", bluetoothManager.gattVerification.tinycardiaService)
            discoveryRow("ECG Stream", bluetoothManager.gattVerification.ecgStream)
            discoveryRow("Inference Result", bluetoothManager.gattVerification.inferenceResult)
            discoveryRow("Device Status", bluetoothManager.gattVerification.deviceStatus)
            discoveryRow("Device Control", bluetoothManager.gattVerification.deviceControl)

            Divider()
            Text("Battery Service").font(.headline)
            discoveryRow("Service", bluetoothManager.gattVerification.batteryService)
            discoveryRow("Battery Level", bluetoothManager.gattVerification.batteryLevel)
        }
    }

    private var statusCard: some View {
        CardView(title: "Status") {
            Text(bluetoothManager.statusMessage)
                .foregroundStyle(TinycardiaTheme.secondaryText)
                .frame(maxWidth: .infinity, alignment: .leading)

            if bluetoothManager.protocolErrorCount > 0 {
                Label(
                    "\(bluetoothManager.protocolErrorCount) malformed protocol update(s)",
                    systemImage: "exclamationmark.triangle.fill"
                )
                .font(.caption)
                .foregroundStyle(TinycardiaTheme.attention)
            }
        }
    }

    @ViewBuilder
    private var streamingButton: some View {
        switch bluetoothManager.streamingState {
        case .live:
            Button("Pause") { bluetoothManager.stopLiveStreaming() }
                .buttonStyle(.bordered)
        case .inactive, .failed:
            Button("Start Live") { bluetoothManager.startLiveStreaming() }
                .buttonStyle(.borderedProminent)
                .disabled(!bluetoothManager.gattVerification.isLiveDataReady)
        case .subscribing, .starting, .stopping:
            ProgressView()
        }
    }

    @ViewBuilder
    private func connectionButton(for device: DiscoveredDevice) -> some View {
        switch bluetoothManager.connectionState {
        case .connected:
            Button("Disconnect") { bluetoothManager.disconnect() }
                .buttonStyle(.bordered)
                .tint(TinycardiaTheme.deepMagenta)
        case .connecting:
            ProgressView()
        case .disconnected, .failed:
            Button("Connect") { bluetoothManager.connect(to: device) }
                .buttonStyle(.borderedProminent)
        }
    }

    private func metric(_ label: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(value).font(.subheadline.monospacedDigit().weight(.semibold))
            Text(label).font(.caption2).foregroundStyle(TinycardiaTheme.secondaryText)
        }
    }

    private func valueRow(_ label: String, _ value: String) -> some View {
        HStack {
            Text(label).foregroundStyle(TinycardiaTheme.secondaryText)
            Spacer()
            Text(value).multilineTextAlignment(.trailing)
        }
        .font(.subheadline)
    }

    private func discoveryRow(_ label: String, _ state: DiscoveryItemState) -> some View {
        HStack {
            Text(label)
            Spacer()
            Label(state.title, systemImage: discoverySymbol(for: state))
                .font(.subheadline)
                .foregroundStyle(discoveryColor(for: state))
        }
    }

    private func statusRow(label: String, value: String, symbol: String, color: Color) -> some View {
        HStack(spacing: 8) {
            Text(label)
            Spacer()
            Label(value, systemImage: symbol).foregroundStyle(color)
        }
    }

    private var formattedWindowDuration: String {
        let seconds = Double(bluetoothManager.ecgSamples.count) / TinycardiaProtocol.sampleRateHz
        return seconds.formatted(.number.precision(.fractionLength(1))) + " s"
    }

    private var livePlaceholderText: String {
        switch bluetoothManager.streamingState {
        case .live: "Waiting for ECG samples…"
        case .failed: "Live stream unavailable"
        case .inactive: "Start the live stream to view ECG"
        case .subscribing, .starting: "Preparing the live stream…"
        case .stopping: "Pausing…"
        }
    }

    private var streamingSymbol: String {
        switch bluetoothManager.streamingState {
        case .live: "waveform.path.ecg"
        case .failed: "exclamationmark.triangle.fill"
        case .inactive: "pause.circle.fill"
        case .subscribing, .starting, .stopping: "ellipsis.circle.fill"
        }
    }

    private var streamingColor: Color {
        switch bluetoothManager.streamingState {
        case .live: TinycardiaTheme.success
        case .failed: TinycardiaTheme.failure
        case .inactive: TinycardiaTheme.tertiaryText
        case .subscribing, .starting, .stopping: TinycardiaTheme.active
        }
    }

    private var connectionSymbol: String {
        switch bluetoothManager.connectionState {
        case .connected: "checkmark.circle.fill"
        case .connecting: "ellipsis.circle.fill"
        case .disconnected: "circle"
        case .failed: "xmark.circle.fill"
        }
    }

    private var connectionColor: Color {
        switch bluetoothManager.connectionState {
        case .connected: TinycardiaTheme.success
        case .connecting: TinycardiaTheme.active
        case .disconnected: TinycardiaTheme.tertiaryText
        case .failed: TinycardiaTheme.failure
        }
    }

    private var quickConnectTitle: String {
        switch bluetoothManager.connectionState {
        case .connected: "Tinycardia connected"
        case .connecting: "Connecting…"
        case .disconnected, .failed:
            bluetoothManager.discoveredDevice == nil ? "Connect Tinycardia" : "Tinycardia found"
        }
    }

    private var quickConnectMessage: String {
        switch bluetoothManager.connectionState {
        case .connected:
            "Your wearable is connected. GATT verification will finish automatically before live ECG begins."
        case .connecting:
            "Keep your iPhone close to the wearable while the connection is established."
        case .disconnected, .failed:
            if let device = bluetoothManager.discoveredDevice {
                "\(device.name) is nearby and ready to connect."
            } else if bluetoothManager.isScanning {
                "Looking for a powered-on Tinycardia wearable nearby…"
            } else {
                "Power on your wearable, keep it nearby, then connect to begin."
            }
        }
    }

    private var quickConnectButtonTitle: String {
        switch bluetoothManager.connectionState {
        case .connected: "Open Live ECG"
        case .connecting: "Connecting…"
        case .disconnected, .failed:
            bluetoothManager.discoveredDevice == nil ? "Find Tinycardia" : "Connect Tinycardia"
        }
    }

    private var quickConnectButtonSymbol: String {
        switch bluetoothManager.connectionState {
        case .connected: "waveform.path.ecg"
        case .connecting: "ellipsis"
        case .disconnected, .failed:
            bluetoothManager.discoveredDevice == nil ? "antenna.radiowaves.left.and.right" : "link"
        }
    }

    private func performPrimaryConnectionAction() {
        switch bluetoothManager.connectionState {
        case .connected:
            selectedSection = .live
        case .connecting:
            break
        case .disconnected, .failed:
            if let device = bluetoothManager.discoveredDevice {
                bluetoothManager.connect(to: device)
            } else {
                bluetoothManager.startScanning()
            }
        }
    }

}

private enum AppSection: String, CaseIterable, Identifiable {
    case live
    case history
    case bluetooth

    var id: String { rawValue }
    var title: String { rawValue.capitalized }
    var symbol: String {
        switch self {
        case .live: "waveform.path.ecg"
        case .history: "clock.arrow.circlepath"
        case .bluetooth: "wave.3.right"
        }
    }
}

private struct FloatingTabBar: View {
    @Binding var selection: AppSection
    let isConnected: Bool

    var body: some View {
        HStack(spacing: 6) {
            ForEach(AppSection.allCases) { section in
                Button {
                    selection = section
                } label: {
                    VStack(spacing: 4) {
                        ZStack(alignment: .topTrailing) {
                            Image(systemName: section.symbol)
                                .font(.system(size: 18, weight: .semibold))
                            if section == .bluetooth, isConnected {
                                Circle()
                                    .fill(TinycardiaTheme.lightPink)
                                    .frame(width: 7, height: 7)
                                    .offset(x: 4, y: -3)
                            }
                        }
                        Text(section.title)
                            .font(.caption2.weight(.semibold))
                    }
                    .foregroundStyle(
                        selection == section ? TinycardiaTheme.lightPink : TinycardiaTheme.secondaryText
                    )
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 9)
                }
                .buttonStyle(.plain)
                .accessibilityAddTraits(selection == section ? .isSelected : [])
            }
        }
        .padding(6)
        .background(TinycardiaTheme.elevatedSurface, in: RoundedRectangle(cornerRadius: 24, style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: 24, style: .continuous)
                .stroke(TinycardiaTheme.white.opacity(0.16), lineWidth: 0.8)
        }
        .shadow(color: .black.opacity(0.13), radius: 20, y: 8)
        .padding(.horizontal, 18)
        .padding(.bottom, 7)
    }
}

struct ECGHistoryView: View {
    @ObservedObject var historyStore: ECGHistoryStore
    @State private var searchText = ""
    @State private var classificationFilter: ClassificationFilter = .all
    @State private var qualityFilter: QualityFilter = .all
    @State private var timeFilter: TimeFilter = .fourteenDays
    @State private var sortOrder: HistorySortOrder = .newest
    @State private var isShowingFilters = false

    private var filteredRecords: [ECGScanRecord] {
        historyStore.records
            .filter { record in
                classificationFilter.matches(record)
                    && qualityFilter.matches(record)
                    && timeFilter.matches(record)
                    && matchesSearch(record)
            }
            .sorted {
                sortOrder == .newest ? $0.capturedAt > $1.capturedAt : $0.capturedAt < $1.capturedAt
            }
    }

    var body: some View {
        NavigationStack {
            Group {
                if historyStore.records.isEmpty {
                    ContentUnavailableView(
                        "No ECG History Yet",
                        systemImage: "waveform.path.ecg.rectangle",
                        description: Text("Completed inference windows will be saved here automatically for 14 days.")
                    )
                } else if filteredRecords.isEmpty {
                    ContentUnavailableView.search(text: searchText)
                } else {
                    List {
                        Section {
                            ForEach(filteredRecords) { record in
                                NavigationLink {
                                    ECGScanDetailView(record: record)
                                } label: {
                                    HistoryRow(record: record)
                                }
                                .listRowBackground(TinycardiaTheme.surface)
                            }
                            .onDelete { offsets in
                                historyStore.delete(at: offsets, from: filteredRecords)
                            }
                        } header: {
                            Text("\(filteredRecords.count) of \(historyStore.records.count) scans")
                        } footer: {
                            Text("Scans older than 14 days are removed automatically.")
                        }
                    }
                    .listStyle(.insetGrouped)
                    .scrollContentBackground(.hidden)
                    .background(TinycardiaTheme.background)
                    .listRowBackground(TinycardiaTheme.surface)
                    .contentMargins(.bottom, 100, for: .scrollContent)
                }
            }
            .navigationTitle("ECG History")
            .searchable(text: $searchText, prompt: "Inference ID, result, quality, device")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button {
                        isShowingFilters = true
                    } label: {
                        Label("Filters", systemImage: activeFilterCount == 0
                            ? "line.3.horizontal.decrease.circle"
                            : "line.3.horizontal.decrease.circle.fill")
                    }
                }
            }
            .sheet(isPresented: $isShowingFilters) {
                HistoryFilterSheet(
                    classificationFilter: $classificationFilter,
                    qualityFilter: $qualityFilter,
                    timeFilter: $timeFilter,
                    sortOrder: $sortOrder
                )
            }
            .safeAreaInset(edge: .bottom) { Color.clear.frame(height: 88) }
            .overlay(alignment: .bottom) {
                if let message = historyStore.persistenceMessage {
                    Text(message)
                        .font(.caption)
                        .foregroundStyle(TinycardiaTheme.lightPink)
                        .padding(8)
                        .background(TinycardiaTheme.elevatedSurface, in: Capsule())
                        .padding(.bottom, 92)
                }
            }
        }
    }

    private var activeFilterCount: Int {
        (classificationFilter == .all ? 0 : 1)
            + (qualityFilter == .all ? 0 : 1)
            + (timeFilter == .fourteenDays ? 0 : 1)
            + (sortOrder == .newest ? 0 : 1)
    }

    private func matchesSearch(_ record: ECGScanRecord) -> Bool {
        guard !searchText.isEmpty else { return true }
        let query = searchText.localizedLowercase
        return String(record.inferenceID).contains(query)
            || record.classification.title.localizedLowercase.contains(query)
            || record.quality.title.localizedLowercase.contains(query)
            || record.deviceName.localizedLowercase.contains(query)
            || record.deviceIdentifier.localizedLowercase.contains(query)
    }
}

private struct HistoryFilterSheet: View {
    @Binding var classificationFilter: ClassificationFilter
    @Binding var qualityFilter: QualityFilter
    @Binding var timeFilter: TimeFilter
    @Binding var sortOrder: HistorySortOrder
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List {
                Section("Inference result") {
                    ForEach(ClassificationFilter.allCases) { option in
                        filterRow(option.title, selected: classificationFilter == option) {
                            classificationFilter = option
                        }
                    }
                }

                Section("Signal quality") {
                    ForEach(QualityFilter.allCases) { option in
                        filterRow(option.title, selected: qualityFilter == option) {
                            qualityFilter = option
                        }
                    }
                }

                Section("Capture time") {
                    ForEach(TimeFilter.allCases) { option in
                        filterRow(option.title, selected: timeFilter == option) {
                            timeFilter = option
                        }
                    }
                }

                Section("Sort order") {
                    ForEach(HistorySortOrder.allCases) { option in
                        filterRow(option.title, selected: sortOrder == option) {
                            sortOrder = option
                        }
                    }
                }
            }
            .scrollContentBackground(.hidden)
            .background(TinycardiaTheme.background)
            .navigationTitle("History Filters")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button("Reset") {
                        classificationFilter = .all
                        qualityFilter = .all
                        timeFilter = .fourteenDays
                        sortOrder = .newest
                    }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                        .fontWeight(.semibold)
                }
            }
        }
        .presentationDetents([.large])
        .presentationDragIndicator(.visible)
        .preferredColorScheme(.dark)
        .tint(TinycardiaTheme.lightPink)
    }

    private func filterRow(_ title: String, selected: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack {
                Text(title)
                    .foregroundStyle(TinycardiaTheme.primaryText)
                Spacer()
                if selected {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(TinycardiaTheme.lightPink)
                }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .listRowBackground(TinycardiaTheme.black)
    }
}

private struct HistoryRow: View {
    let record: ECGScanRecord

    var body: some View {
        HStack(spacing: 12) {
            ZStack {
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(inferenceColor(record.classification).opacity(0.14))
                    .frame(width: 46, height: 46)
                Image(systemName: inferenceSymbol(record.classification))
                    .foregroundStyle(inferenceColor(record.classification))
            }
            VStack(alignment: .leading, spacing: 3) {
                HStack {
                    Text(record.classification.title).font(.headline)
                    if let confidence = record.confidencePercent {
                        Text("\(confidence.formatted(.number.precision(.fractionLength(1))))%")
                            .font(.caption.monospacedDigit())
                            .foregroundStyle(TinycardiaTheme.secondaryText)
                    }
                }
                Text(record.capturedAt, format: .dateTime.month(.abbreviated).day().hour().minute())
                    .font(.subheadline)
                    .foregroundStyle(TinycardiaTheme.secondaryText)
                Text("ID \(record.inferenceID), \(record.quality.title), \(record.durationSeconds.formatted(.number.precision(.fractionLength(1)))) s")
                    .font(.caption)
                    .foregroundStyle(TinycardiaTheme.secondaryText)
            }
        }
        .padding(.vertical, 3)
    }
}

private struct ECGScanDetailView: View {
    let record: ECGScanRecord

    var body: some View {
        ScrollView {
            VStack(spacing: 16) {
                CardView(title: "Archived ECG") {
                    ECGWaveformView(samples: record.samples)
                        .frame(height: 240)
                    HStack {
                        detailMetric("Rate", "\(Int(record.sampleRateHz)) Hz")
                        Spacer()
                        detailMetric("Samples", record.sampleCount.formatted())
                        Spacer()
                        detailMetric("Duration", "\(record.durationSeconds.formatted(.number.precision(.fractionLength(1)))) s")
                    }
                }

                CardView(title: "Inference") {
                    Label(record.classification.title, systemImage: inferenceSymbol(record.classification))
                        .font(.title2.weight(.semibold))
                        .foregroundStyle(inferenceColor(record.classification))
                    detailRow("Confidence", record.confidencePercent.map {
                        $0.formatted(.number.precision(.fractionLength(1))) + "%"
                    } ?? "Unavailable")
                    detailRow("Signal quality", record.quality.title)
                    detailRow("Inference ID", record.inferenceID.formatted())
                    detailRow("Captured", record.capturedAt.formatted(date: .abbreviated, time: .standard))
                    detailRow("Wearable timestamp", "\(record.wearableTimestampMilliseconds) ms")
                    detailRow("Device", record.deviceName)
                }

                Text("Prototype model output. This is not a medical diagnosis.")
                    .font(.caption)
                    .foregroundStyle(TinycardiaTheme.secondaryText)
            }
            .padding()
        }
        .background(TinycardiaTheme.background)
        .navigationTitle("Scan \(record.inferenceID)")
        .navigationBarTitleDisplayMode(.inline)
    }

    private func detailRow(_ label: String, _ value: String) -> some View {
        HStack {
            Text(label).foregroundStyle(TinycardiaTheme.secondaryText)
            Spacer()
            Text(value).multilineTextAlignment(.trailing)
        }
        .font(.subheadline)
    }

    private func detailMetric(_ label: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(value).font(.subheadline.monospacedDigit().weight(.semibold))
            Text(label).font(.caption2).foregroundStyle(TinycardiaTheme.secondaryText)
        }
    }
}

private enum ClassificationFilter: String, CaseIterable, Identifiable {
    case all, normal, atrialFibrillation, unknown
    var id: String { rawValue }
    var title: String {
        switch self {
        case .all: "All results"
        case .normal: "Normal"
        case .atrialFibrillation: "AFib"
        case .unknown: "Unknown"
        }
    }
    func matches(_ record: ECGScanRecord) -> Bool {
        switch self {
        case .all: true
        case .normal: record.classification == .normal
        case .atrialFibrillation: record.classification == .atrialFibrillation
        case .unknown: record.classification == .unknown
        }
    }
}

private enum QualityFilter: String, CaseIterable, Identifiable {
    case all, good, poor, leadOff, unknown
    var id: String { rawValue }
    var title: String {
        switch self {
        case .all: "All quality"
        case .good: "Good"
        case .poor: "Poor"
        case .leadOff: "Lead off"
        case .unknown: "Unknown"
        }
    }
    func matches(_ record: ECGScanRecord) -> Bool {
        switch self {
        case .all: true
        case .good: record.quality == .good
        case .poor: record.quality == .poor
        case .leadOff: record.quality == .leadOff
        case .unknown: record.quality == .unknown
        }
    }
}

private enum TimeFilter: String, CaseIterable, Identifiable {
    case today, sevenDays, fourteenDays
    var id: String { rawValue }
    var title: String {
        switch self {
        case .today: "Last 24 hours"
        case .sevenDays: "Last 7 days"
        case .fourteenDays: "Last 14 days"
        }
    }
    func matches(_ record: ECGScanRecord) -> Bool {
        let interval: TimeInterval = switch self {
        case .today: 24 * 60 * 60
        case .sevenDays: 7 * 24 * 60 * 60
        case .fourteenDays: 14 * 24 * 60 * 60
        }
        return record.capturedAt >= Date.now.addingTimeInterval(-interval)
    }
}

private enum HistorySortOrder: String, CaseIterable, Identifiable {
    case newest, oldest
    var id: String { rawValue }
    var title: String { self == .newest ? "Newest first" : "Oldest first" }
}

struct ECGWaveformView: View {
    let samples: [ECGSample]

    var body: some View {
        Canvas { context, size in
            drawGrid(context: &context, size: size)
            guard samples.count > 1 else { return }

            let values = samples.map { Double($0.rawValue) }
            guard let minimum = values.min(), let maximum = values.max() else { return }
            let midpoint = (minimum + maximum) / 2.0
            let amplitude = max((maximum - minimum) * 0.6, 1.0)

            var path = Path()
            for (index, value) in values.enumerated() {
                let x = size.width * CGFloat(index) / CGFloat(values.count - 1)
                let normalized = (value - midpoint) / amplitude
                let y = size.height * (0.5 - (CGFloat(normalized) * 0.45))
                if index == 0 {
                    path.move(to: CGPoint(x: x, y: y))
                } else {
                    path.addLine(to: CGPoint(x: x, y: y))
                }
            }
            context.stroke(path, with: .color(TinycardiaTheme.lightPink), lineWidth: 1.5)
        }
        .background(Color.black.opacity(0.94), in: RoundedRectangle(cornerRadius: 12))
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .accessibilityLabel("ECG waveform")
        .accessibilityValue("\(samples.count) samples")
    }

    private func drawGrid(context: inout GraphicsContext, size: CGSize) {
        var grid = Path()
        for column in 1..<10 {
            let x = size.width * CGFloat(column) / 10
            grid.move(to: CGPoint(x: x, y: 0))
            grid.addLine(to: CGPoint(x: x, y: size.height))
        }
        for row in 1..<8 {
            let y = size.height * CGFloat(row) / 8
            grid.move(to: CGPoint(x: 0, y: y))
            grid.addLine(to: CGPoint(x: size.width, y: y))
        }
        context.stroke(grid, with: .color(TinycardiaTheme.deepMagenta.opacity(0.55)), lineWidth: 0.5)
    }
}

struct CardView<Content: View>: View {
    let title: String
    @ViewBuilder let content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(title).font(.title3.weight(.semibold))
            content
        }
        .padding()
        .frame(maxWidth: .infinity, alignment: .leading)
        .tinycardiaSurface(cornerRadius: 18)
    }
}

private func inferenceSymbol(_ classification: ECGClassification) -> String {
    switch classification {
    case .normal: "checkmark.heart.fill"
    case .atrialFibrillation: "exclamationmark.heart.fill"
    case .unknown: "questionmark.circle.fill"
    }
}

private func inferenceColor(_ classification: ECGClassification) -> Color {
    switch classification {
    case .normal: TinycardiaTheme.lightPink
    case .atrialFibrillation: TinycardiaTheme.deepMagenta
    case .unknown: TinycardiaTheme.secondaryText
    }
}

private func discoverySymbol(for state: DiscoveryItemState) -> String {
    switch state {
    case .notChecked: "circle"
    case .pending: "ellipsis.circle"
    case .found: "checkmark.circle.fill"
    case .missing: "xmark.circle.fill"
    case .invalidProperties: "exclamationmark.triangle.fill"
    }
}

private func discoveryColor(for state: DiscoveryItemState) -> Color {
    switch state {
    case .notChecked: TinycardiaTheme.tertiaryText
    case .pending: TinycardiaTheme.active
    case .found: TinycardiaTheme.success
    case .missing: TinycardiaTheme.failure
    case .invalidProperties: TinycardiaTheme.attention
    }
}

private func formattedDuration(seconds: UInt32) -> String {
    let hours = seconds / 3_600
    let minutes = (seconds % 3_600) / 60
    let remainingSeconds = seconds % 60
    return hours > 0
        ? "\(hours)h \(minutes)m \(remainingSeconds)s"
        : "\(minutes)m \(remainingSeconds)s"
}
