namespace X3LaptopCompanion
{
    public partial class MainWindow
    {
        public void ToggleMuteFromUi()
        {
            HostLog.Write("Toggle mute requested.");
            RefreshTeamsPresence();
            if (IsTeamsDryRun)
            {
                TeamsText = "Dry run";
                MicrophoneText = "Toggle received";
                CameraText = "Unchanged";
                HostLog.Write("Toggle mute dry-run completed; Teams was not touched.");
                DetailText = "Dry run: X3 mute command received over BLE. Teams was not focused or controlled.";
                QueueHostStatusIfChanged(true, CompanionTriState.Off, CompanionTriState.Unknown,
                    "Dry-run mute received", "toggle dry-run");
                return;
            }

            if (!teamsController.TryToggleMute())
            {
                HostLog.Write("Toggle mute failed; Teams not found.");
                DetailText = "Teams was not found. Start or join a Teams meeting, then try again.";
                QueueHostStatusIfChanged(false, CompanionTriState.Unknown, CompanionTriState.Unknown, "Teams not found",
                    "toggle teams missing");
                return;
            }

            MicrophoneText = "Toggle sent";
            HostLog.Write("Toggle mute sent to Teams.");
            DetailText =
                "Mute toggle sent with Ctrl+Shift+M. State detection will become authoritative once Teams status detection is implemented.";
            QueueHostStatusIfChanged(true, CompanionTriState.Unknown, CompanionTriState.Unknown, "Mute toggle sent",
                "toggle sent");
        }

        private void RefreshTeamsPresence()
        {
            if (IsTeamsDryRun)
            {
                TeamsText = "Dry run";
                CameraText = "Unknown";
                MicrophoneText = "Unknown";
                return;
            }

            TeamsText = teamsController.IsTeamsRunning ? "Running" : "Not detected";
            CameraText = "Unknown";
            MicrophoneText = "Unknown";
        }

        private void ApplyTeamsDryRun()
        {
            HostLog.Write("Apply Teams dry run. enabled=" + IsTeamsDryRun);
            TeamsModeText = IsTeamsDryRun ? "Dry run" : "Live Teams";
            RefreshTeamsPresence();
            DetailText = IsTeamsDryRun
                ? "Teams dry run is active. BLE stays connected, but Teams will not be focused or controlled."
                : "Teams dry run disabled. X3 mute commands will control Teams.";
            if (!IsTestMode)
            {
                SendCurrentHostStatus();
            }
        }
    }
}
