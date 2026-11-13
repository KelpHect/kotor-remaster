//! The family name of a TrueType/OpenType font file (the `name` table), so the settings view can preview
//! the player's chosen native font. Only the first face of a collection is read.

fn u16_at(bytes: &[u8], at: usize) -> Option<u16> {
    Some(u16::from_be_bytes([*bytes.get(at)?, *bytes.get(at + 1)?]))
}

fn u32_at(bytes: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_be_bytes([*bytes.get(at)?, *bytes.get(at + 1)?, *bytes.get(at + 2)?, *bytes.get(at + 3)?]))
}

/// The typographic family (name id 16), else the family (id 1), from Windows Unicode records first.
pub fn family_name(bytes: &[u8]) -> Option<String> {
    let mut face = 0usize;
    if bytes.get(..4) == Some(b"ttcf") {
        face = u32_at(bytes, 12)? as usize;
    }
    let tables = usize::from(u16_at(bytes, face + 4)?);
    let name_table = (0..tables).find_map(|index| {
        let record = face + 12 + index * 16;
        (bytes.get(record..record + 4)? == b"name").then(|| u32_at(bytes, record + 8)).flatten()
    })? as usize;
    let count = usize::from(u16_at(bytes, name_table + 2)?);
    let strings = name_table + usize::from(u16_at(bytes, name_table + 4)?);
    let mut best: Option<(u8, String)> = None;
    for index in 0..count {
        let record = name_table + 6 + index * 12;
        let platform = u16_at(bytes, record)?;
        let name_id = u16_at(bytes, record + 6)?;
        let length = usize::from(u16_at(bytes, record + 8)?);
        let offset = usize::from(u16_at(bytes, record + 10)?);
        if platform != 3 && platform != 0 {
            continue;
        }
        let rank = match name_id {
            16 => 2,
            1 => 1,
            _ => continue,
        };
        let raw = bytes.get(strings + offset..strings + offset + length)?;
        let units: Vec<u16> = raw.as_chunks::<2>().0.iter().map(|pair| u16::from_be_bytes(*pair)).collect();
        let text = String::from_utf16_lossy(&units);
        if !text.is_empty() && best.as_ref().is_none_or(|(have, _)| rank > *have) {
            best = Some((rank, text));
        }
    }
    best.map(|(_, name)| name)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_embedded_barlow() {
        assert_eq!(family_name(crate::assets::FONTS[0]).as_deref(), Some("Barlow"));
        assert_eq!(family_name(crate::assets::FONTS[1]).as_deref(), Some("Barlow"));
        assert_eq!(family_name(b"junk"), None);
        assert_eq!(family_name(&[]), None);
    }
}
