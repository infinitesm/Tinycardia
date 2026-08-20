import SwiftUI

@main
struct TinycardiaApp: App {
    @StateObject private var bluetoothManager = BluetoothManager()
    @StateObject private var historyStore = ECGHistoryStore()

    var body: some Scene {
        WindowGroup {
            AppRootView(bluetoothManager: bluetoothManager, historyStore: historyStore)
        }
    }
}

private struct AppRootView: View {
    @ObservedObject var bluetoothManager: BluetoothManager
    @ObservedObject var historyStore: ECGHistoryStore
    @AppStorage("suppressTinycardiaDemoGuide") private var suppressDemoGuide = false
    @State private var splashOpacity = 0.0
    @State private var appOpacity = 0.0
    @State private var isShowingSplash = true
    @State private var isShowingGettingStarted = true

    var body: some View {
        ZStack {
            Group {
                if isShowingGettingStarted {
                    GettingStartedView(
                        bluetoothManager: bluetoothManager,
                        complete: completeGettingStarted
                    )
                } else {
                    ContentView(bluetoothManager: bluetoothManager, historyStore: historyStore)
                }
            }
            .opacity(appOpacity)

            if isShowingSplash {
                VisientLaunchView()
                    .opacity(splashOpacity)
                    .zIndex(1)
            }
        }
        .background(TinycardiaTheme.black.ignoresSafeArea())
        .tint(TinycardiaTheme.vividPink)
        .preferredColorScheme(.dark)
        .task {
            guard isShowingSplash else { return }

            withAnimation(.easeOut(duration: 0.7)) {
                splashOpacity = 1
            }
            try? await Task.sleep(nanoseconds: 700_000_000)
            try? await Task.sleep(nanoseconds: 3_000_000_000)

            withAnimation(.easeIn(duration: 0.7)) {
                splashOpacity = 0
            }
            try? await Task.sleep(nanoseconds: 700_000_000)
            isShowingGettingStarted = !suppressDemoGuide
            isShowingSplash = false

            withAnimation(.easeInOut(duration: 0.8)) {
                appOpacity = 1
            }
        }
        .onChange(of: bluetoothManager.latestInference) { _, inference in
            guard let inference else { return }
            historyStore.capture(
                inference: inference,
                samples: bluetoothManager.ecgSamples,
                deviceName: bluetoothManager.connectedDeviceName,
                deviceIdentifier: bluetoothManager.discoveredDevice?.id
            )
        }
    }

    private func completeGettingStarted(dontShowAgain: Bool) {
        suppressDemoGuide = dontShowAgain
        withAnimation(.easeInOut(duration: 0.45)) {
            isShowingGettingStarted = false
        }

        if let device = bluetoothManager.discoveredDevice {
            bluetoothManager.connect(to: device)
        } else if !bluetoothManager.isScanning {
            bluetoothManager.startScanning()
        }
    }
}

private struct VisientLaunchView: View {
    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()

            VStack(spacing: 28) {
                HStack(alignment: .bottom, spacing: 18) {
                    Image("VisientLaunch")
                        .resizable()
                        .scaledToFit()
                        .frame(width: 170, height: 164)
                        .accessibilityHidden(true)

                    VStack(alignment: .leading, spacing: 1) {
                        Text("visient")
                        Text("technologies")
                    }
                    .font(.custom("Courier", size: 21))
                    .foregroundStyle(.white)
                    .padding(.bottom, 18)
                }

                VStack(spacing: 4) {
                    Text("made with passion")
                    Text("<3 from kyle")
                }
                .font(.system(size: 16, weight: .regular, design: .default))
                .foregroundStyle(Color(white: 0.72))
                .multilineTextAlignment(.center)
            }
            .accessibilityElement(children: .combine)
        }
    }
}
