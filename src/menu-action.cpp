#include <obs-frontend-api.h>
#include <QAction>
#include <QMetaObject>
#include <QObject>

/* OBS's tools-menu API takes no checked-state pointer, so the item cannot show
 * whether the output is running. A QAction can: it is checkable and the menu
 * draws its state every time it opens. */

extern "C" void *obc_menu_action_create(const char *name,
					void (*on_toggled)(bool));

extern "C" void obc_menu_action_set_checked(void *action_v, bool checked);

extern "C" void *obc_menu_action_create(const char *name,
					void (*on_toggled)(bool))
{
	QAction *action =
		static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(name));
	if (!action)
		return nullptr;

	action->setCheckable(true);
	action->setChecked(false);
	QObject::connect(action, &QAction::toggled,
			 [on_toggled](bool checked) { on_toggled(checked); });
	return action;
}

/* Called from libobs signal callbacks, i.e. never from the GUI thread. */
extern "C" void obc_menu_action_set_checked(void *action_v, bool checked)
{
	QAction *action = static_cast<QAction *>(action_v);
	if (!action)
		return;

	QMetaObject::invokeMethod(
		action, [action, checked] { action->setChecked(checked); },
		Qt::QueuedConnection);
}
