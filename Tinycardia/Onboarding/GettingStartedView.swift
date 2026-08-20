import SwiftUI

struct GettingStartedView: View {
    @ObservedObject var bluetoothManager: BluetoothManager
    let complete: (Bool) -> Void

    @State private var stepIndex = 0
    @State private var dontShowAgain = false

    private let steps = GettingStartedStep.allCases

    var body: some View {
        ZStack {
            TinycardiaTheme.background
            .ignoresSafeArea()

            VStack(spacing: 0) {
                header

                ScrollView {
                    GettingStartedStepView(step: steps[stepIndex])
                        .padding(.horizontal, 20)
                        .padding(.vertical, 18)
                        .id(stepIndex)
                        .transition(.opacity.combined(with: .move(edge: .trailing)))
                }

                footer
            }
        }
        .animation(.easeInOut(duration: 0.25), value: stepIndex)
    }

    private var header: some View {
        HStack {
            Text("Onboarding")
                .font(.title3.weight(.medium))
                .foregroundStyle(TinycardiaTheme.white)
            Spacer()
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 16)
        .background(TinycardiaTheme.black)
    }

    private var footer: some View {
        VStack(spacing: 13) {
            HStack(spacing: 6) {
                ForEach(steps.indices, id: \.self) { index in
                    Capsule()
                        .fill(index <= stepIndex ? TinycardiaTheme.vividPink : TinycardiaTheme.darkBerry.opacity(0.45))
                        .frame(maxWidth: .infinity)
                        .frame(height: 5)
                }
            }

            Toggle(isOn: $dontShowAgain) {
                VStack(alignment: .leading, spacing: 1) {
                    Text("Don’t show this guide again")
                        .font(.subheadline)
                    Text("Leave this off for shared demo devices.")
                        .font(.caption2)
                        .foregroundStyle(TinycardiaTheme.secondaryText)
                }
            }
            .toggleStyle(.switch)

            HStack(spacing: 12) {
                if stepIndex > 0 {
                    Button("Back") {
                        stepIndex -= 1
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.large)
                }

                Button {
                    if stepIndex == steps.count - 1 {
                        complete(dontShowAgain)
                    } else {
                        stepIndex += 1
                    }
                } label: {
                    HStack {
                        Text(stepIndex == steps.count - 1 ? connectionButtonTitle : "Continue")
                        Image(systemName: stepIndex == steps.count - 1 ? "link" : "arrow.right")
                    }
                    .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .tint(TinycardiaTheme.vividPink)
                .controlSize(.large)
            }
        }
        .padding(.horizontal, 20)
        .padding(.top, 13)
        .padding(.bottom, 10)
        .background(TinycardiaTheme.black)
    }

    private var connectionButtonTitle: String {
        bluetoothManager.discoveredDevice == nil ? "Find Tinycardia" : "Connect Tinycardia"
    }
}

private enum GettingStartedStep: CaseIterable {
    case power
    case skinPreparation
    case electrodePlacement
    case liveECG
    case history
}

private struct GettingStartedStepView: View {
    let step: GettingStartedStep

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            VStack(alignment: .leading, spacing: 7) {
                Text(title)
                    .font(.largeTitle.bold())
                Text(subtitle)
                    .font(.body)
                    .foregroundStyle(TinycardiaTheme.secondaryText)
                    .fixedSize(horizontal: false, vertical: true)
            }

            if step != .power {
                visual
            }

            VStack(alignment: .leading, spacing: 12) {
                ForEach(Array(instructions.enumerated()), id: \.offset) { index, instruction in
                    InstructionRow(number: index + 1, text: instruction)
                }
            }

            if let noteTitle, let noteText {
                UserNote(title: noteTitle, text: noteText, symbol: noteSymbol)
            }

            if step == .liveECG {
                InferenceExplanationView()
            }

            if step == .electrodePlacement {
                Text("Placement is shown from the patient’s perspective. When looking at another person, their left side appears on your right.")
                    .font(.caption)
                    .foregroundStyle(TinycardiaTheme.secondaryText)
                    .frame(maxWidth: .infinity, alignment: .center)
                    .multilineTextAlignment(.center)
            }
        }
        .frame(maxWidth: 620, alignment: .leading)
        .frame(maxWidth: .infinity)
    }

    @ViewBuilder
    private var visual: some View {
        switch step {
        case .power:
            EmptyView()
        case .skinPreparation:
            SkinPreparationDiagram()
        case .electrodePlacement:
            ElectrodePlacementDiagram()
        case .liveECG:
            LiveECGDiagram()
        case .history:
            HistoryDiagram()
        }
    }

    private var title: String {
        switch step {
        case .power: "Power and connect the leads"
        case .skinPreparation: "Prepare the skin"
        case .electrodePlacement: "Place the electrodes"
        case .liveECG: "Read the Live ECG screen"
        case .history: "Review previous scans"
        }
    }

    private var subtitle: String {
        switch step {
        case .power:
            "Start with the wearable powered on and its electrode cable firmly connected."
        case .skinPreparation:
            "Clean, dry skin helps the electrodes stay attached and produce a clearer ECG signal."
        case .electrodePlacement:
            "Use the modified Lead II configuration below, matching each lead color to its position."
        case .liveECG:
            "The Live tab shows the ECG waveform and the wearable’s latest result."
        case .history:
            "Every inference and its ECG window are saved automatically so they can be reviewed later."
        }
    }

    private var instructions: [String] {
        switch step {
        case .power:
            [
                "Press and hold the device power button for 3 seconds to turn Tinycardia on or off.",
                "Look for the blue LED on the device. If the blue LED is illuminated, Tinycardia is on.",
                "Push the 3.5 mm electrode jack fully into the device so it is seated all the way."
            ]
        case .skinPreparation:
            [
                "Shave hair from each electrode area when needed so the adhesive can contact the skin.",
                "Remove skin oil and debris using rubbing alcohol or soap and water.",
                "Let the skin dry completely before applying any electrode."
            ]
        case .electrodePlacement:
            [
                "Place the red electrode on the upper left chest, a few inches below the collarbone.",
                "Place the yellow electrode on the upper right chest, a few inches below the collarbone.",
                "Place the green electrode on the lower left side near the edge of the ribcage."
            ]
        case .liveECG:
            [
                "Connect Tinycardia. The app opens Live ECG when the device is ready.",
                "Confirm the waveform is moving and Signal quality reads Good. Consider the result and confidence together, and recheck the electrodes if needed."
            ]
        case .history:
            [
                "Open ECG History from the bottom navigation to see saved scans.",
                "Search by inference ID, result, quality, or device, and filter scans by capture time.",
                "Tap a scan to see its saved 10-second waveform and inference details. Scans are retained for 14 days."
            ]
        }
    }

    private var noteTitle: String? {
        switch step {
        case .power: "Before continuing"
        case .skinPreparation: "Good contact matters"
        case .electrodePlacement: "Use the patient’s left and right"
        case .liveECG: nil
        case .history: "Stored on this iPhone"
        }
    }

    private var noteText: String? {
        switch step {
        case .power:
            "If the blue LED is not on, hold the power button again for a full 3 seconds before trying to connect."
        case .skinPreparation:
            "Do not apply an electrode while the skin is still wet; moisture and poor adhesion can degrade the signal."
        case .electrodePlacement:
            "Upper left and lower left mean the patient’s own left side, not the viewer’s left."
        case .liveECG: nil
        case .history:
            "History is local to the phone. Old scans are removed automatically after 14 days."
        }
    }

    private var noteSymbol: String {
        switch step {
        case .power: "power.circle.fill"
        case .skinPreparation: "hand.raised.fill"
        case .electrodePlacement: "person.crop.circle.badge.checkmark"
        case .liveECG: "exclamationmark.triangle.fill"
        case .history: "iphone"
        }
    }
}

private struct InstructionRow: View {
    let number: Int
    let text: String

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Text(number.formatted())
                .font(.caption.bold())
                .foregroundStyle(TinycardiaTheme.white)
                .frame(width: 25, height: 25)
                .background(TinycardiaTheme.vividPink, in: Circle())
            Text(text)
                .font(.body)
                .fixedSize(horizontal: false, vertical: true)
        }
    }
}

private struct UserNote: View {
    let title: String
    let text: String
    let symbol: String

    var body: some View {
        HStack(alignment: .top, spacing: 11) {
            Image(systemName: symbol)
                .foregroundStyle(TinycardiaTheme.lightPink)
                .font(.title3)
            VStack(alignment: .leading, spacing: 3) {
                Text(title).font(.subheadline.weight(.semibold))
                Text(text)
                    .font(.caption)
                    .foregroundStyle(TinycardiaTheme.secondaryText)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .tinycardiaSurface(cornerRadius: 16)
    }
}

private struct SkinPreparationDiagram: View {
    private let items = [
        ("scissors", "Shave", "Remove hair"),
        ("drop.fill", "Clean", "Remove oil"),
        ("wind", "Dry", "Wait fully")
    ]

    var body: some View {
        HStack(spacing: 9) {
            ForEach(Array(items.enumerated()), id: \.offset) { _, item in
                VStack(spacing: 8) {
                    Image(systemName: item.0)
                        .font(.title2)
                        .foregroundStyle(TinycardiaTheme.lightPink)
                        .frame(height: 30)
                    Text(item.1).font(.subheadline.bold())
                    Text(item.2)
                        .font(.caption2)
                        .foregroundStyle(TinycardiaTheme.secondaryText)
                        .multilineTextAlignment(.center)
                }
                .padding(.vertical, 15)
                .frame(maxWidth: .infinity)
                .tinycardiaSurface(cornerRadius: 18)
            }
        }
        .accessibilityElement(children: .combine)
        .accessibilityLabel("Skin preparation steps: shave, clean, and dry")
    }
}

private struct ElectrodePlacementDiagram: View {
    var body: some View {
        VStack(spacing: 8) {
            HStack {
                Text("PATIENT’S RIGHT")
                Spacer()
                Text("PATIENT’S LEFT")
            }
            .font(.caption2.bold())
            .foregroundStyle(TinycardiaTheme.secondaryText)

            GeometryReader { proxy in
                ZStack {
                    TorsoShape()
                        .fill(TinycardiaTheme.surface)
                        .overlay { TorsoShape().stroke(TinycardiaTheme.border, lineWidth: 1.5) }

                    Path { path in
                        path.move(to: CGPoint(x: proxy.size.width * 0.26, y: proxy.size.height * 0.23))
                        path.addQuadCurve(
                            to: CGPoint(x: proxy.size.width * 0.5, y: proxy.size.height * 0.27),
                            control: CGPoint(x: proxy.size.width * 0.39, y: proxy.size.height * 0.18)
                        )
                        path.addQuadCurve(
                            to: CGPoint(x: proxy.size.width * 0.74, y: proxy.size.height * 0.23),
                            control: CGPoint(x: proxy.size.width * 0.61, y: proxy.size.height * 0.18)
                        )
                    }
                    .stroke(TinycardiaTheme.lightPink.opacity(0.35), lineWidth: 2)

                    ElectrodeMarker(color: electrodeYellow, letter: "Y", label: "Yellow\nupper right", usesDarkText: true)
                        .position(x: proxy.size.width * 0.34, y: proxy.size.height * 0.36)
                    ElectrodeMarker(color: .red, letter: "R", label: "Red\nupper left", usesDarkText: false)
                        .position(x: proxy.size.width * 0.66, y: proxy.size.height * 0.36)
                    ElectrodeMarker(color: .green, letter: "G", label: "Green\nlower left", usesDarkText: false)
                        .position(x: proxy.size.width * 0.67, y: proxy.size.height * 0.73)
                }
            }
            .frame(height: 310)
        }
        .padding(14)
        .tinycardiaSurface(cornerRadius: 22)
        .accessibilityElement(children: .combine)
        .accessibilityLabel("Modified Lead Two placement: yellow on patient upper right chest, red on patient upper left chest, and green on patient lower left ribcage")
    }

    private var electrodeYellow: Color {
        Color(red: 0.95, green: 0.72, blue: 0.04)
    }
}

private struct TorsoShape: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        path.move(to: CGPoint(x: rect.width * 0.42, y: rect.height * 0.05))
        path.addLine(to: CGPoint(x: rect.width * 0.38, y: rect.height * 0.14))
        path.addCurve(
            to: CGPoint(x: rect.width * 0.13, y: rect.height * 0.25),
            control1: CGPoint(x: rect.width * 0.3, y: rect.height * 0.16),
            control2: CGPoint(x: rect.width * 0.2, y: rect.height * 0.17)
        )
        path.addCurve(
            to: CGPoint(x: rect.width * 0.22, y: rect.height * 0.95),
            control1: CGPoint(x: rect.width * 0.18, y: rect.height * 0.55),
            control2: CGPoint(x: rect.width * 0.2, y: rect.height * 0.78)
        )
        path.addLine(to: CGPoint(x: rect.width * 0.78, y: rect.height * 0.95))
        path.addCurve(
            to: CGPoint(x: rect.width * 0.87, y: rect.height * 0.25),
            control1: CGPoint(x: rect.width * 0.8, y: rect.height * 0.78),
            control2: CGPoint(x: rect.width * 0.82, y: rect.height * 0.55)
        )
        path.addCurve(
            to: CGPoint(x: rect.width * 0.62, y: rect.height * 0.14),
            control1: CGPoint(x: rect.width * 0.8, y: rect.height * 0.17),
            control2: CGPoint(x: rect.width * 0.7, y: rect.height * 0.16)
        )
        path.addLine(to: CGPoint(x: rect.width * 0.58, y: rect.height * 0.05))
        path.closeSubpath()
        return path
    }
}

private struct ElectrodeMarker: View {
    let color: Color
    let letter: String
    let label: String
    let usesDarkText: Bool

    var body: some View {
        VStack(spacing: 5) {
            Text(letter)
                .font(.headline.bold())
                .foregroundStyle(usesDarkText ? Color.black : Color.white)
                .frame(width: 38, height: 38)
                .background(color, in: Circle())
                .overlay { Circle().stroke(.white, lineWidth: 3) }
                .shadow(color: .black.opacity(0.18), radius: 4, y: 2)
            Text(label)
                .font(.caption2.bold())
                .multilineTextAlignment(.center)
                .fixedSize()
        }
    }
}

private struct LiveECGDiagram: View {
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("LIVE ECG").font(.caption.bold()).foregroundStyle(TinycardiaTheme.secondaryText)
            MiniWaveform()
                .frame(height: 105)
            HStack {
                Label("Normal", systemImage: "checkmark.heart.fill").foregroundStyle(TinycardiaTheme.lightPink)
                Spacer()
                Text("Confidence 92.4%")
                    .font(.subheadline.monospacedDigit().bold())
            }
            Text("Signal quality: Good")
                .font(.caption)
                .foregroundStyle(TinycardiaTheme.secondaryText)
        }
        .padding(16)
        .tinycardiaSurface(cornerRadius: 22)
        .accessibilityElement(children: .combine)
        .accessibilityLabel("Example Live ECG screen showing a waveform, Normal result, 92.4 percent confidence, and good signal quality")
    }
}

private struct MiniWaveform: View {
    var body: some View {
        Canvas { context, size in
            var grid = Path()
            for column in 1..<8 {
                let x = size.width * CGFloat(column) / 8
                grid.move(to: CGPoint(x: x, y: 0))
                grid.addLine(to: CGPoint(x: x, y: size.height))
            }
            for row in 1..<5 {
                let y = size.height * CGFloat(row) / 5
                grid.move(to: CGPoint(x: 0, y: y))
                grid.addLine(to: CGPoint(x: size.width, y: y))
            }
            context.stroke(grid, with: .color(TinycardiaTheme.deepMagenta.opacity(0.5)), lineWidth: 0.5)

            let points: [CGPoint] = [
                .init(x: 0.00, y: 0.55), .init(x: 0.12, y: 0.55), .init(x: 0.17, y: 0.45),
                .init(x: 0.21, y: 0.62), .init(x: 0.25, y: 0.54), .init(x: 0.34, y: 0.54),
                .init(x: 0.39, y: 0.14), .init(x: 0.44, y: 0.86), .init(x: 0.49, y: 0.54),
                .init(x: 0.61, y: 0.54), .init(x: 0.68, y: 0.39), .init(x: 0.75, y: 0.54),
                .init(x: 1.00, y: 0.54)
            ]
            var waveform = Path()
            for (index, point) in points.enumerated() {
                let scaled = CGPoint(x: point.x * size.width, y: point.y * size.height)
                index == 0 ? waveform.move(to: scaled) : waveform.addLine(to: scaled)
            }
            context.stroke(waveform, with: .color(TinycardiaTheme.lightPink), lineWidth: 2)
        }
        .background(.black, in: RoundedRectangle(cornerRadius: 12))
    }
}

private struct InferenceExplanationView: View {
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Understanding the result")
                .font(.headline)
            inferenceRow("Normal", "No AFib pattern was identified.", TinycardiaTheme.lightPink)
            inferenceRow("AFib", "The result is consistent with AFib.", TinycardiaTheme.deepMagenta)
            inferenceRow("Unknown", "The window could not be classified reliably.", TinycardiaTheme.white.opacity(0.55))
            Divider()
            Text("Confidence shows how strongly the model supports the result. Always check signal quality too. Results are not a medical diagnosis.")
                .font(.caption)
                .foregroundStyle(TinycardiaTheme.secondaryText)
        }
        .padding(14)
        .tinycardiaSurface(cornerRadius: 16)
    }

    private func inferenceRow(_ title: String, _ detail: String, _ color: Color) -> some View {
        HStack(alignment: .top, spacing: 9) {
            Circle().fill(color).frame(width: 9, height: 9).padding(.top, 5)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.subheadline.weight(.semibold))
                Text(detail).font(.caption).foregroundStyle(TinycardiaTheme.secondaryText)
            }
        }
    }
}

private struct HistoryDiagram: View {
    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text("ECG History").font(.headline)
                Spacer()
                Image(systemName: "line.3.horizontal.decrease.circle.fill").foregroundStyle(TinycardiaTheme.lightPink)
            }
            .padding(14)

            Divider()
            historyRow("Normal", "Today, 10:42 AM", "ID 184, Good", TinycardiaTheme.lightPink)
            Divider().padding(.leading, 58)
            historyRow("AFib", "Yesterday, 4:18 PM", "ID 177, Good", TinycardiaTheme.deepMagenta)
            Divider().padding(.leading, 58)
            historyRow("Unknown", "Aug 15, 9:06 AM", "ID 163, Poor", TinycardiaTheme.white.opacity(0.55))
        }
        .tinycardiaSurface(cornerRadius: 22)
        .accessibilityElement(children: .combine)
        .accessibilityLabel("Example ECG History list with Normal, AFib, and Unknown scans")
    }

    private func historyRow(_ title: String, _ date: String, _ detail: String, _ color: Color) -> some View {
        HStack(spacing: 12) {
            Image(systemName: "waveform.path.ecg")
                .foregroundStyle(color)
                .frame(width: 32, height: 32)
                .background(color.opacity(0.12), in: RoundedRectangle(cornerRadius: 9))
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.subheadline.weight(.semibold))
                Text(date).font(.caption).foregroundStyle(TinycardiaTheme.secondaryText)
            }
            Spacer()
            Text(detail).font(.caption2).foregroundStyle(TinycardiaTheme.secondaryText)
        }
        .padding(12)
    }
}
