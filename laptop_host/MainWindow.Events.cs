using System.ComponentModel;
using System.Diagnostics;
using System.Windows;

namespace X3LaptopCompanion
{
    public partial class MainWindow
    {
        private void OnLoaded(object sender, RoutedEventArgs e)
        {
            HostLog.Write("Main window loaded. TestMode=" + IsTestMode);
            RefreshTeamsPresence();
            connectionService.Start();
            statusTimer.Start();
        }

        private void OnClosing(object sender, CancelEventArgs e)
        {
            HostLog.Write("Main window close requested; hiding to tray.");
            e.Cancel = true;
            Hide();
        }

        private void ToggleMute_Click(object sender, RoutedEventArgs e)
        {
            ToggleMuteFromUi();
        }

        private void SimulateX3Mute_Click(object sender, RoutedEventArgs e)
        {
            SimulateX3MutePress();
        }

        private void SendTestStatus_Click(object sender, RoutedEventArgs e)
        {
            SendTestStatus();
        }

        private void OpenLog_Click(object sender, RoutedEventArgs e)
        {
            OpenLog();
        }

        private void Hide_Click(object sender, RoutedEventArgs e)
        {
            Hide();
        }

        public void OpenLog()
        {
            HostLog.Write("Open log requested.");
            var logPath = HostLog.LogPath;
            Process.Start(new ProcessStartInfo
            {
                FileName = "explorer.exe",
                Arguments = "/select,\"" + logPath + "\"",
                UseShellExecute = true
            });
        }
    }
}
