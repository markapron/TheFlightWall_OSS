#pragma once

namespace AirportNameConfiguration
{
    // Compile-time overrides for airport display names.
    // These take precedence over AeroAPI data and the persistent AirportNameCache.
    //
    // Code: use IATA (e.g. "PHL") or ICAO (e.g. "KPHL") — whichever AeroAPI
    // returns as dest_code for that airport (IATA is preferred; ICAO is the
    // fallback for airports that have no IATA code).
    //
    // Name: keep short — the matrix clips at 16 chars (6 px font, 100 px wide text area).
    // Hospital abbreviations are NOT applied to overrides; write the final string directly.
    //
    // Example entries:
    //   { "PHL",  "Philadelphia Intl"  },
    //   { "KPNE", "Northeast Philly"   },
    //   { "N87",  "Solberg-Hunterdon"  },

    struct Entry { const char *code; const char *name; };

    static const Entry kOverrides[] = {
        // Add overrides below. Leave the array empty if none are needed.
        { "16DE", "Nemours CH" }
    };

    static const int kOverrideCount =
        (int)(sizeof(kOverrides) / sizeof(kOverrides[0]));

} // namespace AirportNameConfiguration
