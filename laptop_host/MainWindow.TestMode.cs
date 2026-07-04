using System.Windows;

namespace X3LaptopCompanion
{
    public partial class MainWindow
    {
        private CompanionTriState testMicrophone = CompanionTriState.Unknown;
        private CompanionTriState testCamera = CompanionTriState.Unknown;
        private ushort simulatedButtonSequence = 0xFF00;

        public void SimulateX3MutePress()
        {
            HostLog.Write("Simulate X3 mute press requested.");
            if (!IsTestMode)
            {
                DetailText = "Enable test mode before simulating X3 commands.";
                return;
            }

            simulatedButtonSequence++;
            OnButtonEventReceived(this, new CompanionButtonEvent(CompanionButton.ToggleMute,
                CompanionButtonAction.Released, simulatedButtonSequence, 0));
        }

        public void SendTestStatus()
        {
            HostLog.Write("Test status requested.");
            if (IsTestMode)
            {
                SendTestHostStatus("manual");
                return;
            }

            if (!IsTeamsDryRun)
            {
                RefreshTeamsPresence();
            }
            else
            {
                TeamsText = "Dry run";
            }

            MicrophoneText = "Muted";
            CameraText = "Off";
            if (IsTestMode)
            {
                DetailText = "Test status shown locally. BLE writes are disabled while test mode is on.";
                return;
            }

            DetailText = "Test status queued for BLE if it changed.";
            QueueHostStatusIfChanged(true, CompanionTriState.Off, CompanionTriState.Off, "BLE test status",
                "manual test status");
        }

        private void CycleTestMicrophone_Click(object sender, RoutedEventArgs e)
        {
            testMicrophone = NextTriState(testMicrophone);
            TestMicrophoneToggleText = TriStateText(testMicrophone, "Muted", "Live");
            OnTestStatusChanged("microphone");
        }

        private void CycleTestCamera_Click(object sender, RoutedEventArgs e)
        {
            testCamera = NextTriState(testCamera);
            TestCameraToggleText = TriStateText(testCamera, "Off", "Active");
            OnTestStatusChanged("camera");
        }

        private void ApplyTestMode()
        {
            HostLog.Write("Apply test mode. enabled=" + IsTestMode);
            if (IsTestMode)
            {
                TestModeText = "On";
                ApplyTestStatusToUi();
                DetailText = "Test mode is active. BLE stays connected and sends the simulated status to the X3.";
                connectionService.Start();
                SendTestHostStatus("enabled");
                return;
            }

            TestModeText = "Off";
            ConnectionText = "Disconnected";
            DetailText = "Test mode disabled. Scanning for the X3 companion service.";
            connectionService.Start();
        }

        private void OnTestStatusChanged(string reason)
        {
            if (!IsTestMode)
            {
                return;
            }

            SendTestHostStatus(reason);
        }

        private void SendTestHostStatus(string reason, bool force = false)
        {
            ApplyTestStatusToUi();
            var message = string.IsNullOrWhiteSpace(TestMessage) ? "Test status" : TestMessage;
            DetailText = "Test mode sent " + reason + ": teams=" + TestTeamsToggleText +
                ", mic=" + TestMicrophoneToggleText + ", camera=" + TestCameraToggleText + ".";
            QueueHostStatusIfChanged(TestTeamsDetected, testMicrophone, testCamera, message, "test " + reason, force);
        }

        private void ApplyTestStatusToUi()
        {
            TeamsText = TestTeamsDetected ? "Detected (test)" : "Not detected (test)";
            MicrophoneText = TestMicrophoneToggleText + " (test)";
            CameraText = TestCameraToggleText + " (test)";
        }

        private static CompanionTriState NextTriState(CompanionTriState value)
        {
            if (value == CompanionTriState.Unknown)
            {
                return CompanionTriState.Off;
            }

            return value == CompanionTriState.Off ? CompanionTriState.On : CompanionTriState.Unknown;
        }

        private static string TriStateText(CompanionTriState value, string offText, string onText)
        {
            if (value == CompanionTriState.Off)
            {
                return offText;
            }

            return value == CompanionTriState.On ? onText : "Unknown";
        }
    }
}
