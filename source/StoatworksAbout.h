/*
 * Stoatworks Labs - About window data for intaglio.
 *
 * PROVISIONAL, hand-written in the shape stoatworks-backend/scripts/sync-about.py
 * generates. Intaglio is not yet registered in the website's projects.json, so
 * the sync cannot produce this file; once it is, `sync-about.py --only intaglio
 * --apply` overwrites this with the generated copy. The facts below are the ones
 * that registration will carry, so the parameter count -- which depends on how
 * many links are present -- does not change when it does.
 *
 * `guide` is deliberately empty: there is no user guide yet, and a link that is
 * not written is left out rather than shown as a button that opens a 404. That
 * is what keeps the About block at four parameters; writing a guide makes it
 * five, and Intaglio.cpp static_asserts the run so the change cannot land
 * silently.
 */
#pragma once

namespace stoatworks::about
{
    inline constexpr auto name = "intaglio";
    inline constexpr auto slug = "intaglio";
    inline constexpr auto hook = "Engraved line shading for Resolume";
    inline constexpr auto licence = "MIT";
    inline constexpr auto guide = "";
    inline constexpr auto page = "https://stoatworks-labs.com/software/intaglio/";
    inline constexpr auto repo = "https://github.com/stoatworks-labs/intaglio";
    inline constexpr auto versionFallback = "v0.1.0";

    inline constexpr auto org = "Stoatworks Labs";
    inline constexpr auto home = "https://stoatworks-labs.com";
    inline constexpr auto tagline = "Open tools for the people who run the show.";

    /* The canonical funding links, matching FUNDING.yml and the support footer. */
    struct Link { const char* name; const char* url; };
    inline constexpr Link funding[] = {
        { "GitHub Sponsors", "https://github.com/sponsors/stoatworks-labs" },
        { "Ko-fi", "https://ko-fi.com/stoatworkslabs" },
        { "Patreon", "https://patreon.com/StoatworksLabs" },
        { "Liberapay", "https://liberapay.com/stoatworks-labs" },
    };
}
