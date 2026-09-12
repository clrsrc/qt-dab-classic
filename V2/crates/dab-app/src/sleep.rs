//! Sleep-Timer (Entscheidung 18): Minuten frei waehlbar, Aktion "stumm"
//! oder "beenden". Laeuft im Sekundentakt aus [`App::tick`](crate::App::tick)
//! mit; die Restzeit zeigt die Statusleiste aus `until_unix`.

use crate::app::{App, AppError, AppEvent, Effects};
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum SleepAction {
    Mute,
    Quit,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct SleepState {
    pub minutes: u32,
    pub action: SleepAction,
    pub set_at: i64,
    pub until_unix: i64,
}

#[derive(Debug, Default)]
pub struct Sleep {
    pub state: Option<SleepState>,
}

impl App {
    pub fn sleep_set(&mut self, minutes: u32, action: SleepAction, now: i64) -> Result<Effects, AppError> {
        if minutes == 0 || minutes > 24 * 60 {
            return Err(AppError::Other("minutes out of range".into()));
        }
        self.sleep.state = Some(SleepState { minutes, action, set_at: now, until_unix: now + minutes as i64 * 60 });
        let mut fx = Effects::default();
        fx.events.push(AppEvent::SleepChanged { sleep: self.sleep.state.clone() });
        Ok(fx)
    }

    pub fn sleep_cancel(&mut self) -> Effects {
        let mut fx = Effects::default();
        if self.sleep.state.take().is_some() {
            fx.events.push(AppEvent::SleepChanged { sleep: None });
        }
        fx
    }

    pub fn sleep_state(&self) -> Option<&SleepState> {
        self.sleep.state.as_ref()
    }

    /// Abgelaufen: stumm schalten bzw. Beenden melden (das Fenster schliesst).
    pub fn sleep_tick(&mut self, now: i64) -> Effects {
        let mut fx = Effects::default();
        let Some(s) = self.sleep.state.clone() else { return fx };
        if now < s.until_unix {
            return fx;
        }
        self.sleep.state = None;
        if s.action == SleepAction::Mute {
            fx.append(self.set_mute(true));
        }
        fx.events.push(AppEvent::SleepChanged { sleep: None });
        fx.events.push(AppEvent::SleepElapsed { action: s.action });
        fx
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{DataDirs, Presets, Settings};
    use dab_api::Command;

    fn app() -> App {
        let tmp = std::env::temp_dir().join(format!("dabclassic-sleep-{}", std::process::id()));
        App::with(DataDirs::with_root(&tmp, true), Settings::default(), Presets::default())
    }

    #[test]
    fn mute_after_minutes() {
        let mut a = app();
        assert!(a.sleep_set(0, SleepAction::Mute, 1000).is_err());
        let fx = a.sleep_set(2, SleepAction::Mute, 1000).unwrap();
        assert!(matches!(fx.events[0], AppEvent::SleepChanged { sleep: Some(_) }));
        assert_eq!(a.sleep_state().unwrap().until_unix, 1120);
        assert!(a.sleep_tick(1119).commands.is_empty());
        let fx = a.sleep_tick(1120);
        assert_eq!(fx.commands, vec![Command::SetMute { muted: true }]);
        assert!(fx.events.iter().any(|e| matches!(e, AppEvent::SleepElapsed { action: SleepAction::Mute })));
        assert!(a.sleep_state().is_none());
        assert!(a.state.muted);
    }

    #[test]
    fn quit_and_cancel() {
        let mut a = app();
        a.sleep_set(1, SleepAction::Quit, 0).unwrap();
        let fx = a.sleep_cancel();
        assert!(matches!(fx.events[0], AppEvent::SleepChanged { sleep: None }));
        assert!(a.sleep_cancel().events.is_empty());
        a.sleep_set(1, SleepAction::Quit, 0).unwrap();
        let fx = a.sleep_tick(60);
        assert!(fx.commands.is_empty());
        assert!(fx.events.iter().any(|e| matches!(e, AppEvent::SleepElapsed { action: SleepAction::Quit })));
    }
}
