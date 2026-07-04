using System;
using System.Diagnostics;
using System.Linq;
using System.Runtime.InteropServices;

namespace X3LaptopCompanion
{
    public sealed class TeamsController
    {
        private const int SW_RESTORE = 9;
        private const uint INPUT_KEYBOARD = 1;
        private const ushort KEYEVENTF_KEYUP = 0x0002;
        private const ushort VK_CONTROL = 0x11;
        private const ushort VK_SHIFT = 0x10;
        private const ushort VK_M = 0x4D;

        public bool IsTeamsRunning
        {
            get { return FindTeamsProcess() != null; }
        }

        public bool TryToggleMute()
        {
            var process = FindTeamsProcess();
            if (process == null)
            {
                return false;
            }

            if (process.MainWindowHandle != IntPtr.Zero)
            {
                ShowWindow(process.MainWindowHandle, SW_RESTORE);
                SetForegroundWindow(process.MainWindowHandle);
            }

            SendCtrlShiftM();
            return true;
        }

        private static Process FindTeamsProcess()
        {
            var candidates = Process.GetProcesses()
                .Where(p =>
                {
                    try
                    {
                        var name = p.ProcessName ?? string.Empty;
                        return name.IndexOf("Teams", StringComparison.OrdinalIgnoreCase) >= 0 ||
                               name.IndexOf("ms-teams", StringComparison.OrdinalIgnoreCase) >= 0;
                    }
                    catch
                    {
                        return false;
                    }
                })
                .ToList();

            return candidates.FirstOrDefault(p => p.MainWindowHandle != IntPtr.Zero) ?? candidates.FirstOrDefault();
        }

        private static void SendCtrlShiftM()
        {
            var inputs = new[]
            {
                KeyDown(VK_CONTROL),
                KeyDown(VK_SHIFT),
                KeyDown(VK_M),
                KeyUp(VK_M),
                KeyUp(VK_SHIFT),
                KeyUp(VK_CONTROL)
            };

            SendInput((uint)inputs.Length, inputs, Marshal.SizeOf(typeof(INPUT)));
        }

        private static INPUT KeyDown(ushort key)
        {
            return new INPUT
            {
                type = INPUT_KEYBOARD,
                ki = new KEYBDINPUT { wVk = key }
            };
        }

        private static INPUT KeyUp(ushort key)
        {
            return new INPUT
            {
                type = INPUT_KEYBOARD,
                ki = new KEYBDINPUT { wVk = key, dwFlags = KEYEVENTF_KEYUP }
            };
        }

        [DllImport("user32.dll")]
        private static extern bool SetForegroundWindow(IntPtr hWnd);

        [DllImport("user32.dll")]
        private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);

        [StructLayout(LayoutKind.Sequential)]
        private struct INPUT
        {
            public uint type;
            public KEYBDINPUT ki;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct KEYBDINPUT
        {
            public ushort wVk;
            public ushort wScan;
            public uint dwFlags;
            public uint time;
            public IntPtr dwExtraInfo;
        }
    }
}
