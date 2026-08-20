import SwiftUI

enum TinycardiaTheme {
    static let lightPink = Color(red: 235 / 255, green: 103 / 255, blue: 192 / 255)
    static let vividPink = Color(red: 212 / 255, green: 79 / 255, blue: 168 / 255)
    static let magenta = Color(red: 189 / 255, green: 57 / 255, blue: 146 / 255)
    static let deepMagenta = Color(red: 169 / 255, green: 36 / 255, blue: 125 / 255)
    static let darkBerry = Color(red: 146 / 255, green: 13 / 255, blue: 102 / 255)
    static let white = Color.white
    static let black = Color.black

    static let background = black
    static let surface = black
    static let elevatedSurface = black
    static let border = white.opacity(0.14)
    static let primaryText = white
    static let secondaryText = white.opacity(0.68)
    static let tertiaryText = white.opacity(0.48)

    static let success = lightPink
    static let active = vividPink
    static let attention = magenta
    static let failure = deepMagenta
}

extension View {
    func tinycardiaSurface(cornerRadius: CGFloat = 18) -> some View {
        background(
            TinycardiaTheme.surface,
            in: RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
        )
        .overlay {
            RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                .stroke(TinycardiaTheme.border, lineWidth: 0.8)
        }
    }
}
