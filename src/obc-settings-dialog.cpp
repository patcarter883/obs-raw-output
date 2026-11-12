/*
 * open-broadcast-raw-output - the operator's settings dialog.
 *
 * The output is registered with no service, so OBS never shows its properties
 * anywhere; the Tools menu is the only way in. This is that way in: a small
 * Qt dialog holding the two things an operator can meaningfully set, which
 * obc_start_output() then reads back out of the plugin's own config.
 *
 * Deliberately plain Qt - no Q_OBJECT, no custom signals - so the build needs
 * no moc step. The C side sees exactly one function, obc_settings_dialog_run().
 */

#include <QByteArray>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QString>
#include <QWidget>

#include <obs-frontend-api.h>

#include <cstddef>
#include <cstdio>

class ObcSettingsDialog : public QDialog {
public:
	ObcSettingsDialog(const QString &target, bool audio, QWidget *parent)
		: QDialog(parent)
	{
		setWindowTitle("Open Broadcast raw output");
		setMinimumWidth(460);

		auto *form = new QFormLayout(this);

		target_edit_ = new QLineEdit(target, this);
		target_edit_->setPlaceholderText("host:port");
		form->addRow("Target (the encoder listens here)", target_edit_);

		auto *hint = new QLabel(
			"The encoder also starts and stops this output, on the port "
			"above the target. The Tools menu stays the manual "
			"override.",
			this);
		hint->setWordWrap(true);
		form->addRow(QString(), hint);

		audio_box_ = new QCheckBox(
			"Send audio, synchronised with the video", this);
		audio_box_->setChecked(audio);
		form->addRow(QString(), audio_box_);

		auto *buttons = new QDialogButtonBox(
			QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
		form->addRow(buttons);
		QObject::connect(buttons, &QDialogButtonBox::accepted, this,
				 &QDialog::accept);
		QObject::connect(buttons, &QDialogButtonBox::rejected, this,
				 &QDialog::reject);
	}

	QString target() const { return target_edit_->text().trimmed(); }
	bool audio() const { return audio_box_->isChecked(); }

private:
	QLineEdit *target_edit_ = nullptr;
	QCheckBox *audio_box_ = nullptr;
};

/* Fills `target`/`audio` and returns true only when the operator accepted.
 * Must be called from the GUI thread (obs_frontend_add_tools_menu_item's
 * callback already runs there). */
extern "C" bool obc_settings_dialog_run(char *target, size_t target_len,
					bool *audio)
{
	if (target == nullptr || target_len == 0 || audio == nullptr)
		return false;

	QWidget *parent =
		static_cast<QWidget *>(obs_frontend_get_main_window());
	ObcSettingsDialog dialog(QString::fromUtf8(target), *audio, parent);
	if (dialog.exec() != QDialog::Accepted)
		return false;

	const QByteArray new_target = dialog.target().toUtf8();
	/* An empty target would leave the output with nowhere to send; treat it
	 * as a cancel rather than accepting a setting that cannot work. */
	if (new_target.isEmpty())
		return false;

	snprintf(target, target_len, "%s", new_target.constData());
	*audio = dialog.audio();
	return true;
}
