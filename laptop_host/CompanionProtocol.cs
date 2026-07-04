using System;

namespace X3LaptopCompanion
{
    public static class CompanionProtocol
    {
        public static readonly Guid ServiceUuid = Guid.Parse("7d2d5f00-778d-4df6-a6d5-7c4e7a000001");
        public static readonly Guid HostTeamsStateUuid = Guid.Parse("7d2d5f00-778d-4df6-a6d5-7c4e7a000002");
        public static readonly Guid HostMicrophoneStateUuid = Guid.Parse("7d2d5f00-778d-4df6-a6d5-7c4e7a000003");
        public static readonly Guid HostCameraStateUuid = Guid.Parse("7d2d5f00-778d-4df6-a6d5-7c4e7a000004");
        public static readonly Guid HostStatusMessageUuid = Guid.Parse("7d2d5f00-778d-4df6-a6d5-7c4e7a000005");
        public static readonly Guid ButtonEventUuid = Guid.Parse("7d2d5f00-778d-4df6-a6d5-7c4e7a000006");
        public static readonly Guid DeviceInfoUuid = Guid.Parse("7d2d5f00-778d-4df6-a6d5-7c4e7a000007");

        public const byte ProtocolVersion = 1;
    }

    public enum CompanionButton : byte
    {
        ToggleMute = 1
    }

    public enum CompanionButtonAction : byte
    {
        Released = 1
    }

    public enum CompanionTriState : byte
    {
        Unknown = 0,
        Off = 1,
        On = 2
    }

    public sealed class CompanionButtonEvent
    {
        public CompanionButtonEvent(CompanionButton button, CompanionButtonAction action, ushort sequence, uint deviceUptimeMs)
        {
            Button = button;
            Action = action;
            Sequence = sequence;
            DeviceUptimeMs = deviceUptimeMs;
        }

        public CompanionButton Button { get; }
        public CompanionButtonAction Action { get; }
        public ushort Sequence { get; }
        public uint DeviceUptimeMs { get; }
    }
}
