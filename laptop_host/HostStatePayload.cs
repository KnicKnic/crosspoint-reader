using System;

namespace X3LaptopCompanion
{
    internal sealed class HostStatePayload
    {
        public HostStatePayload(bool teamsDetected, CompanionTriState microphone, CompanionTriState camera, string message)
        {
            TeamsDetected = teamsDetected;
            Microphone = microphone;
            Camera = camera;
            Message = string.IsNullOrWhiteSpace(message) ? string.Empty : message;
        }

        public bool TeamsDetected { get; }
        public CompanionTriState Microphone { get; }
        public CompanionTriState Camera { get; }
        public string Message { get; }

        public bool SameAs(HostStatePayload other)
        {
            return other != null &&
                TeamsDetected == other.TeamsDetected &&
                Microphone == other.Microphone &&
                Camera == other.Camera &&
                string.Equals(Message, other.Message, StringComparison.Ordinal);
        }
    }
}
