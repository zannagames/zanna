' ===----------------------------------------------------------------------===
'
' Part of the Zanna project, under the GNU GPL v3.
' See LICENSE for license information.
'
' ===----------------------------------------------------------------------===
'
' File: src/tests/fixtures/runtime/test_basic_services_platform.bas
' Purpose: End-to-end check of Zanna.Services from BASIC on the VM and in native
'          binaries: the neutral Platform lifecycle, identity and launch
'          parameters, polled events, non-blocking requests with leaderboard
'          entries, achievements (with icons and global percentages) and stats
'          with their stored events, presence, the overlay, text input, cloud
'          files, the timeline, action input, the Workshop, and the unavailable
'          path when no redistributable exists.
' Key invariants:
'   - ZANNA_SERVICES_TEST_EXPECT selects the scenario: "available" runs
'     against the fake steam_api library named by ZANNA_SERVICES_STEAM_LIBRARY;
'     "missing" points that variable at a file that does not exist.
'   - Every query must return a neutral value when no provider is started.
'   - Prints "RESULT: ok" only when every check passes.
' Ownership/Lifetime:
'   - Results, requests, and byte buffers are owned by module variables.
' Links: src/tests/fixtures/runtime/test_services_platform.zia,
'        src/tests/runtime/RTServicesFakeSteamApi.c,
'        docs/adr/0352-platform-services-runtime-loaded-providers.md,
'        docs/adr/0353-platform-services-player-features.md,
'        docs/adr/0364-platform-services-achievement-icons-and-percentages.md,
'        docs/adr/0365-platform-services-action-input.md,
'        docs/adr/0366-platform-services-workshop.md
'
' ===----------------------------------------------------------------------===

DIM fails AS INTEGER
fails = 0

SUB Check(cond AS BOOLEAN, message AS STRING)
    IF NOT cond THEN
        PRINT "FAIL: "; message
        fails = fails + 1
    END IF
END SUB

SUB CheckNeutral(label AS STRING)
    Check(NOT Zanna.Services.Platform.IsAvailable, label + ": IsAvailable must be false")
    Check(Zanna.Services.Platform.Provider = "", label + ": Provider must be empty")
    Check(Zanna.Services.Platform.UserName = "", label + ": UserName must be empty")
    Check(NOT Zanna.Services.Platform.IsLicensed, label + ": IsLicensed must be false")
    Check(NOT Zanna.Services.Platform.HasFeature(Zanna.Services.Feature.Identity), label + ": no Identity feature")
    Check(NOT Zanna.Services.Platform.HasFeature(Zanna.Services.Feature.LaunchParameters), label + ": no LaunchParameters feature")
    Check(Zanna.Services.Platform.LaunchCommandLine = "", label + ": no launch command line")
    Check(Zanna.Services.Platform.LaunchParameter("team") = "", label + ": no launch parameter")
    Check(Zanna.Services.Timeline.AddEvent("Home run", "", "", 1, 0.0, Zanna.Services.TimelineClip.None) = "", label + ": no timeline events")
    Check(Zanna.Services.Platform.PollEvent() = Zanna.Services.EventKind.None, label + ": no events")
    Check(NOT Zanna.Services.Steam.IsActive, label + ": Steam must be inactive")
    Check(NOT Zanna.Services.Achievements.Unlock("ACH_WIN_ONE_GAME"), label + ": no achievements")
    Check(Zanna.Services.Achievements.IconRgba("ACH_WIN_ONE_GAME").Length = 0, label + ": no achievement icons")
    Check(Zanna.Services.Achievements.GlobalPercent("ACH_WIN_ONE_GAME") = 0.0, label + ": no global percentages")
    Check(NOT Zanna.Services.ActionInput.Start(""), label + ": no action input")
    Check(NOT Zanna.Services.ActionInput.IsPressed("", "swing"), label + ": no controllers")
    Check(Zanna.Services.Workshop.SubscribedCount = 0, label + ": no Workshop items")
    Check(Zanna.Services.Stats.GetInt("NumGames") = 0, label + ": no stats")
    Check(NOT Zanna.Services.Presence.Set("status", "Managing Boston"), label + ": no presence")
    Check(NOT Zanna.Services.Cloud.Exists("league.sav"), label + ": no cloud files")
    DIM board AS Zanna.Services.Request
    board = Zanna.Services.Leaderboards.Download("HOME_RUNS", Zanna.Services.LeaderboardScope.Friends, 0, 0)
    Check(board.IsDone AND NOT board.Succeeded, label + ": leaderboard request must fail")
    Check(board.Error = "Services: no platform services provider is started", label + ": request error " + board.Error)
END SUB

FUNCTION Pump(request AS Zanna.Services.Request) AS BOOLEAN
    DIM frames AS INTEGER
    frames = 0
    DO WHILE NOT request.IsDone AND frames < 4
        Zanna.Services.Platform.Update()
        frames = frames + 1
    LOOP
    Pump = request.IsDone
END FUNCTION

SUB RunMissing()
    DIM result AS OBJECT
    result = Zanna.Services.Platform.Init("steam", "480")
    Check(result.IsErr, "Init must fail without a redistributable")
    Check(Zanna.String.StartsWith(result.UnwrapErrStr(), "Steam: steam_api library not found: "), "unexpected Init error")
    Check(Zanna.Services.Platform.Status = Zanna.Services.Status.LibraryNotFound, "Status must be LibraryNotFound")
    CheckNeutral("missing")
    DIM request AS Zanna.Services.Request
    request = Zanna.Services.Platform.RequestPlayerCount()
    Check(request.IsDone AND NOT request.Succeeded, "request must fail immediately without a provider")
    Zanna.Services.Platform.Shutdown()
    Check(Zanna.Services.Platform.Status = Zanna.Services.Status.NotStarted, "Shutdown must reset Status")
END SUB

SUB RunAvailable()
    DIM result AS OBJECT
    result = Zanna.Services.Platform.Init("steam", "480")
    Check(result.IsOk, "Init must succeed against the fake redistributable")
    IF result.IsErr THEN
        PRINT "Init error: "; result.UnwrapErrStr()
        EXIT SUB
    END IF
    Check(result.UnwrapStr() = "steam", "Init must return the provider id")
    Check(Zanna.Services.Platform.Status = Zanna.Services.Status.Ok, "Status must be Ok")
    Check(Zanna.Services.Platform.UserId = "76561198000000001", "unexpected UserId " + Zanna.Services.Platform.UserId)
    Check(Zanna.Services.Platform.UserName = "Zanna Tester", "unexpected UserName " + Zanna.Services.Platform.UserName)
    Check(Zanna.Services.Platform.Language = "english", "unexpected Language")
    Check(Zanna.Services.Platform.IsDlcInstalled("1234567"), "DLC 1234567 must be installed")
    Check(Zanna.Services.Platform.LaunchCommandLine = "+join 76561198000000002", "unexpected LaunchCommandLine")
    Check(Zanna.Services.Platform.LaunchParameter("season") = "1972", "launch parameter season")
    Check(Zanna.Services.Platform.DlcCount = 2, "two DLC defined")
    Check(Zanna.Services.Platform.DlcNameAt(0) = "Stadium Pack", "first DLC name")
    Check(Zanna.Services.Platform.BuildId = 21042026, "build id")
    Check(Zanna.Services.Platform.BranchName = "public-beta", "branch name")
    Check(Zanna.Services.Steam.HardwareType = Zanna.Services.SteamHardware.SteamFrame, "hardware must be SteamFrame")

    DIM players AS Zanna.Services.Request
    players = Zanna.Services.Platform.RequestPlayerCount()
    Check(NOT players.IsDone, "request must be pending before the pump")
    Zanna.Services.Platform.Update()
    Check(Zanna.Services.Platform.PollEvent() = Zanna.Services.EventKind.ServiceConnected, "first event")
    Check(Zanna.Services.Platform.PollEvent() = Zanna.Services.EventKind.OverlayChanged, "second event")
    Check(Zanna.Services.Platform.EventFlag, "overlay must report open")
    Check(Zanna.Services.Platform.PollEvent() = Zanna.Services.EventKind.DlcInstalled, "third event")
    Check(Zanna.Services.Platform.EventText = "1234567", "DLC event text")
    Check(Zanna.Services.Platform.PollEvent() = Zanna.Services.EventKind.None, "queue must be empty")
    Check(players.IsDone AND players.Succeeded, "player count must complete")
    Check(players.Value = 42, "player count must be 42")

    Check(Zanna.Services.Achievements.Count = 3, "three achievements")
    Check(Zanna.Services.Achievements.Unlock("ACH_WIN_ONE_GAME"), "unlock")
    Check(Zanna.Services.Achievements.IsUnlocked("ACH_WIN_ONE_GAME"), "unlocked")
    Check(Zanna.Services.Stats.SetInt("NumGames", 7), "SetInt")
    Check(Zanna.Services.Stats.GetInt("NumGames") = 7, "GetInt")
    Check(Zanna.Services.Stats.Store(), "Store")
    Zanna.Services.Platform.Update()
    Check(Zanna.Services.Platform.PollEvent() = Zanna.Services.EventKind.AchievementStored, "AchievementStored event")
    Check(Zanna.Services.Platform.EventText = "ACH_WIN_ONE_GAME", "stored achievement id")
    Check(Zanna.Services.Platform.PollEvent() = Zanna.Services.EventKind.StatsStored, "StatsStored event")

    Check(Zanna.Services.Achievements.IconWidth("ACH_WIN_ONE_GAME") = 0, "icon must load on demand")
    Zanna.Services.Platform.Update()
    Check(Zanna.Services.Platform.PollEvent() = Zanna.Services.EventKind.AchievementIconReady, "AchievementIconReady event")
    Check(Zanna.Services.Platform.EventText = "ACH_WIN_ONE_GAME" AND Zanna.Services.Platform.EventFlag, "icon ready payload")
    DIM icon AS Zanna.Collections.Bytes
    icon = Zanna.Services.Achievements.IconRgba("ACH_WIN_ONE_GAME")
    Check(icon.Length = 16 AND icon.Get(0) = 32, "icon pixels")
    DIM percentages AS Zanna.Services.Request
    percentages = Zanna.Services.Achievements.RequestGlobalPercentages()
    Check(Pump(percentages) AND percentages.Succeeded, "global percentages: " + percentages.Error)
    Check(Zanna.Services.Achievements.GlobalPercent("ACH_WIN_ONE_GAME") = 62.5, "global percent")

    DIM upload AS Zanna.Services.Request
    upload = Zanna.Services.Leaderboards.Upload("HOME_RUNS", 75, TRUE)
    Check(Pump(upload) AND upload.Succeeded, "upload: " + upload.Error)
    Check(upload.Value = 1 AND upload.Flag, "upload rank and change")
    DIM download AS Zanna.Services.Request
    download = Zanna.Services.Leaderboards.Download("HOME_RUNS", Zanna.Services.LeaderboardScope.Global, 1, 3)
    Check(Pump(download) AND download.Succeeded, "download: " + download.Error)
    Check(download.EntryCount = 3, "three entries")
    IF download.EntryCount = 3 THEN
        Check(download.EntryRank(0) = 1 AND download.EntryScore(0) = 75, "first entry")
        Check(download.EntryUserName(1) = "Slugger", "second entry name")
    END IF

    Check(Zanna.Services.Presence.Set("status", "Managing Boston"), "presence")
    Zanna.Services.Presence.Clear()
    Check(Zanna.Services.Overlay.Open(Zanna.Services.OverlayPage.Achievements), "overlay page")
    DIM entered AS Zanna.Services.Request
    entered = Zanna.Services.OnScreenKeyboard.RequestText("Team name", "Sox", 32, Zanna.Services.TextInputMode.SingleLine)
    Check(Pump(entered) AND entered.Succeeded, "text input: " + entered.Error)
    Check(entered.Text = "Home Nine", "entered text " + entered.Text)

    DIM saved AS Zanna.Collections.Bytes
    saved = Zanna.Collections.Bytes.FromStr("season 1972")
    Check(Zanna.Services.Cloud.Write("league.sav", saved), "cloud write")
    DIM read AS OBJECT
    read = Zanna.Services.Cloud.Read("league.sav")
    Check(read.IsOk, "cloud read")
    IF read.IsOk THEN
        DIM data AS Zanna.Collections.Bytes
        data = read.Unwrap()
        Check(data.ToStr() = "season 1972", "cloud contents")
    END IF
    Check(Zanna.Services.Cloud.Delete("league.sav"), "cloud delete")

    Check(Zanna.Services.Timeline.SetPhaseId("game-1972-034"), "timeline phase id")
    DIM homer AS STRING
    homer = Zanna.Services.Timeline.AddEvent("Home run", "Two-run shot", "steam_star", 900, -3.0, Zanna.Services.TimelineClip.Featured)
    Check(homer <> "", "timeline event id")
    DIM phaseRecording AS Zanna.Services.Request
    phaseRecording = Zanna.Services.Timeline.RequestPhaseRecording("game-1972-034")
    Check(Pump(phaseRecording) AND phaseRecording.Succeeded, "timeline phase recording: " + phaseRecording.Error)
    Check(phaseRecording.DetailCount = 4, "timeline phase recording details")
    IF phaseRecording.DetailCount = 4 THEN
        Check(phaseRecording.Detail(0) = 90000 AND phaseRecording.Detail(2) = 2, "timeline recorded milliseconds and clips")
    END IF

    DIM manifestDir AS STRING
    manifestDir = Zanna.IO.TempFile.CreateDir()
    DIM manifest AS STRING
    manifest = Zanna.IO.Path.Join(manifestDir, "input_manifest.vdf")
    Zanna.IO.File.WriteAllText(manifest, CHR$(34) + "Action Manifest" + CHR$(34))
    Check(Zanna.Services.ActionInput.Start(manifest), "action input start")
    Check(Zanna.Services.ActionInput.ControllerCount = 2, "two controllers")
    DIM pad AS STRING
    pad = Zanna.Services.ActionInput.ControllerIdAt(1)
    Check(Zanna.Services.ActionInput.ControllerType(pad) = Zanna.Services.ControllerType.PlayStation5, "controller type")
    Check(Zanna.Services.ActionInput.ActivateActionSet("", "menu"), "activate action set")
    Check(Zanna.Services.ActionInput.IsActionActive(pad, "select"), "select active in menu")
    Check(Zanna.Services.ActionInput.OriginCount(pad, "menu", "cursor") = 1, "cursor origin count")
    Check(Zanna.Services.ActionInput.OriginLabel(Zanna.Services.ActionInput.OriginAt(pad, "", "select", 0)) = "Cross Button", "origin label")
    Check(Zanna.Services.ActionInput.ShowBindingPanel(pad), "binding panel")
    Check(Zanna.Services.ActionInput.Stop(), "action input stop")
    Zanna.IO.Dir.RemoveAll(manifestDir)

    Check(Zanna.Services.Workshop.SubscribedCount = 2, "two subscribed Workshop items")
    DIM parks AS STRING
    parks = Zanna.Services.Workshop.SubscribedIdAt(1)
    Check(Zanna.Services.Workshop.NeedsUpdate(parks), "parks item needs a download")
    Check(Zanna.Services.Workshop.Download(parks, TRUE), "Workshop download")
    Zanna.Services.Platform.Update()
    Check(Zanna.Services.Platform.PollEvent() = Zanna.Services.EventKind.WorkshopItemInstalled, "WorkshopItemInstalled event")
    Check(Zanna.Services.Platform.EventText = parks, "installed item id")
    Check(Zanna.Services.Platform.PollEvent() = Zanna.Services.EventKind.WorkshopItemDownloaded, "WorkshopItemDownloaded event")
    Check(Zanna.Services.Workshop.IsInstalled(parks), "parks item installed")
    DIM details AS Zanna.Services.Request
    details = Zanna.Services.Workshop.QueryItems(parks)
    Check(Pump(details) AND details.Succeeded AND details.ItemCount = 1, "Workshop details: " + details.Error)
    IF details.ItemCount = 1 THEN
        DIM parksItem AS Zanna.Services.WorkshopItem
        parksItem = details.ItemAt(0)
        Check(parksItem.Title = "Classic Parks Pack" AND parksItem.Tags = "parks", "Workshop item details")
    END IF

    Zanna.Services.Platform.Shutdown()
    Check(Zanna.Services.Platform.Status = Zanna.Services.Status.NotStarted, "Shutdown must reset Status")
    CheckNeutral("after shutdown")
END SUB

Check(Zanna.Services.Platform.Status = Zanna.Services.Status.NotStarted, "Status must start as NotStarted")
Check(Zanna.Services.Platform.HasProvider("steam"), "steam provider must be compiled in")
CheckNeutral("before init")

DIM expectation AS STRING
expectation = Zanna.System.Environment.GetVariable("ZANNA_SERVICES_TEST_EXPECT")
IF expectation = "missing" THEN
    RunMissing()
ELSEIF expectation = "available" THEN
    RunAvailable()
ELSE
    Check(FALSE, "ZANNA_SERVICES_TEST_EXPECT must be 'missing' or 'available'")
END IF

IF fails = 0 THEN
    PRINT "RESULT: ok"
ELSE
    PRINT "RESULT: "; fails; " failure(s)"
END IF
