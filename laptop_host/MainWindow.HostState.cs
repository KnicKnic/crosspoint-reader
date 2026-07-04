using System.Threading.Tasks;

namespace X3LaptopCompanion
{
    public partial class MainWindow
    {
        private ushort? lastToggleMuteButtonSequence;
        private bool wasBleConnected;
        private bool hostStateWriteInFlight;
        private HostStatePayload lastSentHostState;
        private HostStatePayload pendingHostState;
        private string pendingHostStateReason;

        private void OnConnectionStatusChanged(object sender, CompanionConnectionStatus status)
        {
            Dispatcher.Invoke(() =>
            {
                HostLog.Write("UI status received. connected=" + status.IsConnected + " message=" + status.Message);
                ConnectionText = status.IsConnected ? "Connected" : "Disconnected";
                DetailText = status.Message;
                lastToggleMuteButtonSequence = null;
                var reconnected = status.IsConnected && !wasBleConnected;
                wasBleConnected = status.IsConnected;
                if (!status.IsConnected)
                {
                    ResetHostStateWriteCache();
                    return;
                }

                if (reconnected)
                {
                    SendCurrentHostStatus(force: true);
                }
            });
        }

        private void OnButtonEventReceived(object sender, CompanionButtonEvent buttonEvent)
        {
            HostLog.Write("UI button event received. button=" + buttonEvent.Button +
                " action=" + buttonEvent.Action + " seq=" + buttonEvent.Sequence +
                " deviceUptimeMs=" + buttonEvent.DeviceUptimeMs);
            if (buttonEvent.Button != CompanionButton.ToggleMute ||
                buttonEvent.Action != CompanionButtonAction.Released)
            {
                return;
            }

            if (lastToggleMuteButtonSequence.HasValue &&
                lastToggleMuteButtonSequence.Value == buttonEvent.Sequence)
            {
                HostLog.Write("Duplicate toggle mute button event ignored. seq=" + buttonEvent.Sequence);
                return;
            }

            lastToggleMuteButtonSequence = buttonEvent.Sequence;
            Dispatcher.Invoke(ToggleMuteFromUi);
        }

        private void OnStatusTimerTick(object sender, System.EventArgs e)
        {
            if (IsTestMode)
            {
                ApplyTestStatusToUi();
                SendTestHostStatus("timer");
                return;
            }

            RefreshTeamsPresence();
            SendCurrentHostStatus();
        }

        private void SendCurrentHostStatus(bool force = false)
        {
            if (IsTestMode)
            {
                SendTestHostStatus("current", force);
                return;
            }

            if (IsTeamsDryRun)
            {
                QueueHostStatusIfChanged(true, CompanionTriState.Unknown, CompanionTriState.Unknown,
                    "Teams dry run", "current dry-run", force);
                return;
            }

            QueueHostStatusIfChanged(teamsController.IsTeamsRunning, CompanionTriState.Unknown,
                CompanionTriState.Unknown, teamsController.IsTeamsRunning ? "Teams running" : "Teams not found",
                "current", force);
        }

        private void QueueHostStatusIfChanged(bool teamsDetected, CompanionTriState microphone, CompanionTriState camera,
            string message, string reason, bool force = false)
        {
            if (!wasBleConnected)
            {
                HostLog.Write("Host state write skipped while BLE is disconnected. reason=" + reason);
                return;
            }

            var payload = new HostStatePayload(teamsDetected, microphone, camera, message);
            if (!force && payload.SameAs(lastSentHostState))
            {
                HostLog.Write("Host state unchanged; BLE write skipped. reason=" + reason);
                return;
            }

            if (hostStateWriteInFlight)
            {
                pendingHostState = payload;
                pendingHostStateReason = reason;
                HostLog.Write("Host state write deferred while previous write is in flight. reason=" + reason);
                return;
            }

            _ = SendHostStatePayloadAsync(payload, reason);
        }

        private async Task SendHostStatePayloadAsync(HostStatePayload payload, string reason)
        {
            hostStateWriteInFlight = true;
            try
            {
                HostLog.Write("Host state write queued. reason=" + reason);
                var sent = await connectionService.SendHostStatusAsync(payload.TeamsDetected, payload.Microphone,
                    payload.Camera, payload.Message);
                if (sent)
                {
                    lastSentHostState = payload;
                }
            }
            finally
            {
                hostStateWriteInFlight = false;
                var pending = pendingHostState;
                var pendingReason = pendingHostStateReason;
                pendingHostState = null;
                pendingHostStateReason = null;
                if (pending != null && !pending.SameAs(lastSentHostState))
                {
                    _ = SendHostStatePayloadAsync(pending, pendingReason ?? "pending");
                }
            }
        }

        private void ResetHostStateWriteCache()
        {
            lastSentHostState = null;
            pendingHostState = null;
            pendingHostStateReason = null;
            hostStateWriteInFlight = false;
        }
    }
}
