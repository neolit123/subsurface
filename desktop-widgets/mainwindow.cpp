// SPDX-License-Identifier: GPL-2.0
/*
 * mainwindow.cpp
 *
 * classes for the main UI window in Subsurface
 */
#include "mainwindow.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QDesktopWidget>
#include <QSettings>
#include <QShortcut>
#include <QToolBar>
#include <QStatusBar>
#include <QNetworkProxy>
#include <QUndoStack>
#include <QtConcurrentRun>

#include "core/color.h"
#include "core/divecomputer.h"
#include "core/divesitehelpers.h"
#include "core/gettextfromc.h"
#include "core/git-access.h"
#include "core/isocialnetworkintegration.h"
#include "core/import-csv.h"
#include "core/planner.h"
#include "core/pluginmanager.h"
#include "core/qthelper.h"
#include "core/subsurface-string.h"
#include "core/version.h"
#include "core/windowtitleupdate.h"

#include "core/settings/qPrefCloudStorage.h"
#include "core/settings/qPrefDisplay.h"
#include "core/settings/qPrefPartialPressureGas.h"
#include "core/settings/qPrefTechnicalDetails.h"

#include "core/subsurface-qt/DiveListNotifier.h"

#include "desktop-widgets/about.h"
#include "desktop-widgets/command.h"
#include "desktop-widgets/divecomputermanagementdialog.h"
#include "desktop-widgets/divelistview.h"
#include "desktop-widgets/divelogexportdialog.h"
#include "desktop-widgets/divelogimportdialog.h"
#include "desktop-widgets/diveplanner.h"
#include "desktop-widgets/downloadfromdivecomputer.h"
#include "desktop-widgets/findmovedimagesdialog.h"
#include "desktop-widgets/locationinformation.h"
#include "desktop-widgets/mapwidget.h"
#include "desktop-widgets/subsurfacewebservices.h"
#include "desktop-widgets/tab-widgets/maintab.h"
#include "desktop-widgets/updatemanager.h"
#include "desktop-widgets/usersurvey.h"

#include "profile-widget/profilewidget2.h"

#ifndef NO_PRINTING
#include <QPrintDialog>
#include <QBuffer>
#include "desktop-widgets/printdialog.h"
#endif

#include "qt-models/cylindermodel.h"
#include "qt-models/divepicturemodel.h"
#include "qt-models/diveplannermodel.h"
#include "qt-models/filtermodels.h"
#include "qt-models/tankinfomodel.h"
#include "qt-models/weightsysteminfomodel.h"
#include "qt-models/yearlystatisticsmodel.h"
#include "preferences/preferencesdialog.h"

#ifndef NO_USERMANUAL
#include "usermanual.h"
#endif

#if defined(FBSUPPORT)
#include "plugins/facebook/facebook_integration.h"
#include "plugins/facebook/facebookconnectwidget.h"
#endif

namespace {
	QProgressDialog *progressDialog = nullptr;
	bool progressDialogCanceled = false;
	int progressCounter = 0;

	int round_int (double value) {
		return static_cast<int>(lrint(value));
	};
}


extern "C" int updateProgress(const char *text)
{
	if (verbose)
		qDebug() << "git storage:" << text;
	if (progressDialog) {
		// apparently we don't always get enough space to show the full label
		// so let's manually make enough space (but don't shrink the existing size)
		int width = QFontMetrics(qApp->font()).width(text) + 100;
		if (width > progressDialog->width())
			progressDialog->resize(width + 20, progressDialog->height());
		progressDialog->setLabelText(text);
		progressDialog->setValue(++progressCounter);
		if (progressCounter == 100)
			progressCounter = 0; // yes this is silly, but we really don't know how long it will take
	}
	qApp->processEvents();
	return progressDialogCanceled;
}

MainWindow *MainWindow::m_Instance = nullptr;

extern "C" void showErrorFromC(char *buf)
{
	QString error(buf);
	free(buf);
	emit MainWindow::instance()->showError(error);
}

MainWindow::MainWindow() : QMainWindow(),
	actionNextDive(nullptr),
	actionPreviousDive(nullptr),
#ifndef NO_USERMANUAL
	helpView(0),
#endif
	state(VIEWALL),
	survey(nullptr),
	findMovedImagesDialog(nullptr)
{
	Q_ASSERT_X(m_Instance == NULL, "MainWindow", "MainWindow recreated!");
	m_Instance = this;
	ui.setupUi(this);
	read_hashes();
	// Define the States of the Application Here, Currently the states are situations where the different
	// widgets will change on the mainwindow.

	// for the "default" mode
	mainTab = new MainTab(this);
	diveList = new DiveListView(this);
	graphics = new ProfileWidget2(this);
	MapWidget *mapWidget = MapWidget::instance();

	divePlannerSettingsWidget = new PlannerSettingsWidget(this);
	divePlannerWidget = new DivePlannerWidget(this);
	plannerDetails = new PlannerDetails(this);

	// what is a sane order for those icons? we should have the ones the user is
	// most likely to want towards the top so they are always visible
	// and the ones that someone likely sets and then never touches again towards the bottom
	profileToolbarActions = { ui.profCalcCeiling, ui.profCalcAllTissues, // start with various ceilings
				ui.profIncrement3m, ui.profDcCeiling,
				ui.profPhe, ui.profPn2, ui.profPO2, // partial pressure graphs
				ui.profRuler, ui.profScaled, // measuring and scaling
				ui.profTogglePicture, ui.profTankbar,
				ui.profMod, ui.profNdl_tts, // various values that a user is either interested in or not
				ui.profEad, ui.profSAC,
				ui.profHR, // very few dive computers support this
				ui.profTissues}; // maybe less frequently used

	QToolBar *toolBar = new QToolBar();
	Q_FOREACH (QAction *a, profileToolbarActions)
		toolBar->addAction(a);
	toolBar->setOrientation(Qt::Vertical);
	toolBar->setIconSize(QSize(24,24));
	QWidget *profileContainer = new QWidget();
	QHBoxLayout *profLayout = new QHBoxLayout();
	profLayout->setSpacing(0);
	profLayout->setMargin(0);
	profLayout->setContentsMargins(0,0,0,0);
	profLayout->addWidget(toolBar);
	profLayout->addWidget(graphics);
	profileContainer->setLayout(profLayout);

	diveSiteEdit = new LocationInformationWidget(this);
	connect(diveSiteEdit, &LocationInformationWidget::endEditDiveSite,
			this, &MainWindow::setDefaultState);
	connect(diveSiteEdit, SIGNAL(endEditDiveSite()), this, SLOT(refreshDisplay()));
	connect(diveSiteEdit, &LocationInformationWidget::endEditDiveSite,
			mainTab, &MainTab::refreshDisplayedDiveSite);

	std::pair<QByteArray, QVariant> enabled = std::make_pair("enabled", QVariant(true));
	std::pair<QByteArray, QVariant> disabled = std::make_pair("enabled", QVariant(false));
	PropertyList enabledList;
	PropertyList disabledList;
	enabledList.push_back(enabled);
	disabledList.push_back(disabled);

	registerApplicationState("Default", mainTab, profileContainer, diveList, mapWidget );
	registerApplicationState("AddDive", mainTab, profileContainer, diveList, mapWidget );
	registerApplicationState("EditDive", mainTab, profileContainer, diveList, mapWidget );
	registerApplicationState("PlanDive", divePlannerWidget, profileContainer, divePlannerSettingsWidget, plannerDetails );
	registerApplicationState("EditPlannedDive", divePlannerWidget, profileContainer, diveList, mapWidget );
	registerApplicationState("EditDiveSite", diveSiteEdit, profileContainer, diveList, mapWidget);

	setStateProperties("Default", enabledList, enabledList, enabledList,enabledList);
	setStateProperties("AddDive", enabledList, enabledList, enabledList,enabledList);
	setStateProperties("EditDive", enabledList, enabledList, enabledList,enabledList);
	setStateProperties("PlanDive", enabledList, enabledList, enabledList,enabledList);
	setStateProperties("EditPlannedDive", enabledList, enabledList, enabledList,enabledList);
	setStateProperties("EditDiveSite", enabledList, disabledList, disabledList, enabledList);

	setApplicationState("Default");

	ui.multiFilter->hide();

	setWindowIcon(QIcon(":subsurface-icon"));
	if (!QIcon::hasThemeIcon("window-close")) {
		QIcon::setThemeName("subsurface");
	}
	connect(&diveListNotifier, &DiveListNotifier::selectionChanged, this, &MainWindow::selectionChanged);
	connect(PreferencesDialog::instance(), SIGNAL(settingsChanged()), this, SLOT(readSettings()));
	connect(PreferencesDialog::instance(), SIGNAL(settingsChanged()), diveList, SLOT(update()));
	connect(PreferencesDialog::instance(), SIGNAL(settingsChanged()), diveList, SLOT(reloadHeaderActions()));
	connect(PreferencesDialog::instance(), SIGNAL(settingsChanged()), mainTab, SLOT(updateDiveInfo()));
	connect(PreferencesDialog::instance(), SIGNAL(settingsChanged()), divePlannerWidget, SLOT(settingsChanged()));
	connect(PreferencesDialog::instance(), SIGNAL(settingsChanged()), divePlannerSettingsWidget, SLOT(settingsChanged()));
	connect(PreferencesDialog::instance(), SIGNAL(settingsChanged()), TankInfoModel::instance(), SLOT(update()));
	for (int i = 0; i < NUM_RECENT_FILES; i++) {
		actionsRecent[i] = new QAction(this);
		actionsRecent[i]->setData(i);
		ui.menuFile->insertAction(ui.actionQuit, actionsRecent[i]);
		connect(actionsRecent[i], SIGNAL(triggered(bool)), this, SLOT(recentFileTriggered(bool)));
	}
	ui.menuFile->insertSeparator(ui.actionQuit);
	connect(mainTab, SIGNAL(addDiveFinished()), graphics, SLOT(setProfileState()));
	connect(mainTab, SIGNAL(dateTimeChanged()), graphics, SLOT(dateTimeChanged()));
	connect(DivePlannerPointsModel::instance(), SIGNAL(planCreated()), this, SLOT(planCreated()));
	connect(DivePlannerPointsModel::instance(), SIGNAL(planCanceled()), this, SLOT(planCanceled()));
	connect(DivePlannerPointsModel::instance(), SIGNAL(variationsComputed(QString)), this, SLOT(updateVariations(QString)));
	connect(plannerDetails->printPlan(), SIGNAL(pressed()), divePlannerWidget, SLOT(printDecoPlan()));
	connect(this, SIGNAL(startDiveSiteEdit()), this, SLOT(on_actionDiveSiteEdit_triggered()));
	// connect(information(), SIGNAL(diveSiteChanged(struct dive_site *)), mapWidget, SLOT(centerOnDiveSite(struct dive_site *)));
	connect(this, &MainWindow::showError, ui.mainErrorMessage, &NotificationWidget::showError, Qt::AutoConnection);

	connect(&windowTitleUpdate, &WindowTitleUpdate::updateTitle, this, &MainWindow::setAutomaticTitle);
#ifdef NO_PRINTING
	plannerDetails->printPlan()->hide();
	ui.menuFile->removeAction(ui.actionPrint);
#endif
	enableDisableCloudActions();

	ui.mainErrorMessage->hide();
	graphics->setEmptyState();
	initialUiSetup();
	readSettings();
	diveList->reload(DiveTripModel::TREE);
	diveList->reloadHeaderActions();
	diveList->setFocus();
	MapWidget::instance()->reload();
	diveList->expand(diveList->model()->index(0, 0));
	diveList->scrollTo(diveList->model()->index(0, 0), QAbstractItemView::PositionAtCenter);
	divePlannerWidget->settingsChanged();
	divePlannerSettingsWidget->settingsChanged();
#ifdef NO_USERMANUAL
	ui.menuHelp->removeAction(ui.actionUserManual);
#endif
	memset(&copyPasteDive, 0, sizeof(copyPasteDive));
	memset(&what, 0, sizeof(what));

	updateManager = new UpdateManager(this);
	undoAction = Command::undoAction(this);
	redoAction = Command::redoAction(this);
	undoAction->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_Z));
	redoAction->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_Z));
	ui.menu_Edit->addActions({ undoAction, redoAction });

#ifndef NO_PRINTING
	// copy the bundled print templates to the user path
	QStringList templateBackupList;
	QString templatePathUser(getPrintingTemplatePathUser());
	copy_bundled_templates(getPrintingTemplatePathBundle(), templatePathUser, &templateBackupList);
	if (templateBackupList.length()) {
		QMessageBox msgBox(this);
		templatePathUser.replace("\\", "/");
		templateBackupList.replaceInStrings(templatePathUser + "/", "");
		msgBox.setWindowTitle(tr("Template backup created"));
		msgBox.setText(tr("The following backup printing templates were created:\n\n%1\n\n"
			"Location:\n%2\n\n"
			"Please note that as of this version of Subsurface the default templates\n"
			"are read-only and should not be edited directly, since the application\n"
			"can overwrite them on startup.").arg(templateBackupList.join("\n")).arg(templatePathUser));
		msgBox.setStandardButtons(QMessageBox::Ok);
		msgBox.exec();
	}
	set_bundled_templates_as_read_only();
	find_all_templates();
#endif

	setupSocialNetworkMenu();
	set_git_update_cb(&updateProgress);
	set_error_cb(&showErrorFromC);

	// Toolbar Connections related to the Profile Update
	auto tec = qPrefTechnicalDetails::instance();
	connect(ui.profCalcAllTissues, &QAction::triggered, tec, &qPrefTechnicalDetails::set_calcalltissues);
	connect(ui.profCalcCeiling,    &QAction::triggered, tec, &qPrefTechnicalDetails::set_calcceiling);
	connect(ui.profDcCeiling,      &QAction::triggered, tec, &qPrefTechnicalDetails::set_dcceiling);
	connect(ui.profEad,            &QAction::triggered, tec, &qPrefTechnicalDetails::set_ead);
	connect(ui.profIncrement3m,    &QAction::triggered, tec, &qPrefTechnicalDetails::set_calcceiling3m);
	connect(ui.profMod,            &QAction::triggered, tec, &qPrefTechnicalDetails::set_mod);
	connect(ui.profNdl_tts,        &QAction::triggered, tec, &qPrefTechnicalDetails::set_calcndltts);
	connect(ui.profHR,             &QAction::triggered, tec, &qPrefTechnicalDetails::set_hrgraph);
	connect(ui.profRuler,          &QAction::triggered, tec, &qPrefTechnicalDetails::set_rulergraph);
	connect(ui.profSAC,            &QAction::triggered, tec, &qPrefTechnicalDetails::set_show_sac);
	connect(ui.profScaled,         &QAction::triggered, tec, &qPrefTechnicalDetails::set_zoomed_plot);
	connect(ui.profTogglePicture,  &QAction::triggered, tec, &qPrefTechnicalDetails::set_show_pictures_in_profile);
	connect(ui.profTankbar,        &QAction::triggered, tec, &qPrefTechnicalDetails::set_tankbar);
	connect(ui.profTissues,        &QAction::triggered, tec, &qPrefTechnicalDetails::set_percentagegraph);

	connect(ui.profTissues,        &QAction::triggered, this, &MainWindow::unsetProfHR);
	connect(ui.profHR,             &QAction::triggered, this, &MainWindow::unsetProfTissues);

	auto pp_gas = qPrefPartialPressureGas::instance();
	connect(ui.profPhe, &QAction::triggered, pp_gas, &qPrefPartialPressureGas::set_phe);
	connect(ui.profPn2, &QAction::triggered, pp_gas, &qPrefPartialPressureGas::set_pn2);
	connect(ui.profPO2, &QAction::triggered, pp_gas, &qPrefPartialPressureGas::set_po2);

	connect(tec, &qPrefTechnicalDetails::calcalltissuesChanged        , graphics, &ProfileWidget2::actionRequestedReplot);
	connect(tec, &qPrefTechnicalDetails::calcceilingChanged           , graphics, &ProfileWidget2::actionRequestedReplot);
	connect(tec, &qPrefTechnicalDetails::dcceilingChanged             , graphics, &ProfileWidget2::actionRequestedReplot);
	connect(tec, &qPrefTechnicalDetails::eadChanged                   , graphics, &ProfileWidget2::actionRequestedReplot);
	connect(tec, &qPrefTechnicalDetails::calcceiling3mChanged         , graphics, &ProfileWidget2::actionRequestedReplot);
	connect(tec, &qPrefTechnicalDetails::modChanged                   , graphics, &ProfileWidget2::actionRequestedReplot);
	connect(tec, &qPrefTechnicalDetails::calcndlttsChanged            , graphics, &ProfileWidget2::actionRequestedReplot);
	connect(tec, &qPrefTechnicalDetails::hrgraphChanged               , graphics, &ProfileWidget2::actionRequestedReplot);
	connect(tec, &qPrefTechnicalDetails::rulergraphChanged            , graphics, &ProfileWidget2::actionRequestedReplot);
	connect(tec, &qPrefTechnicalDetails::show_sacChanged              , graphics, &ProfileWidget2::actionRequestedReplot);
	connect(tec, &qPrefTechnicalDetails::zoomed_plotChanged           , graphics, &ProfileWidget2::actionRequestedReplot);
	connect(tec, &qPrefTechnicalDetails::show_pictures_in_profileChanged , graphics, &ProfileWidget2::actionRequestedReplot);
	connect(tec, &qPrefTechnicalDetails::tankbarChanged               , graphics, &ProfileWidget2::actionRequestedReplot);
	connect(tec, &qPrefTechnicalDetails::percentagegraphChanged       , graphics, &ProfileWidget2::actionRequestedReplot);

	connect(pp_gas, &qPrefPartialPressureGas::pheChanged, graphics, &ProfileWidget2::actionRequestedReplot);
	connect(pp_gas, &qPrefPartialPressureGas::pn2Changed, graphics, &ProfileWidget2::actionRequestedReplot);
	connect(pp_gas, &qPrefPartialPressureGas::po2Changed, graphics, &ProfileWidget2::actionRequestedReplot);

	// now let's set up some connections
	connect(graphics, &ProfileWidget2::enableToolbar ,this, &MainWindow::setEnabledToolbar);
	connect(graphics, &ProfileWidget2::disableShortcuts, this, &MainWindow::disableShortcuts);
	connect(graphics, &ProfileWidget2::enableShortcuts, this, &MainWindow::enableShortcuts);
	connect(graphics, &ProfileWidget2::refreshDisplay, this, &MainWindow::refreshDisplay);
	connect(graphics, &ProfileWidget2::editCurrentDive, this, &MainWindow::editCurrentDive);
	connect(graphics, &ProfileWidget2::updateDiveInfo, mainTab, &MainTab::updateDiveInfo);

	connect(PreferencesDialog::instance(), SIGNAL(settingsChanged()), graphics, SLOT(settingsChanged()));

	ui.profCalcAllTissues->setChecked(qPrefTechnicalDetails::calcalltissues());
	ui.profCalcCeiling->setChecked(qPrefTechnicalDetails::calcceiling());
	ui.profDcCeiling->setChecked(qPrefTechnicalDetails::dcceiling());
	ui.profEad->setChecked(qPrefTechnicalDetails::ead());
	ui.profIncrement3m->setChecked(qPrefTechnicalDetails::calcceiling3m());
	ui.profMod->setChecked(qPrefTechnicalDetails::mod());
	ui.profNdl_tts->setChecked(qPrefTechnicalDetails::calcndltts());
	ui.profPhe->setChecked(pp_gas->phe());
	ui.profPn2->setChecked(pp_gas->pn2());
	ui.profPO2->setChecked(pp_gas->po2());
	ui.profHR->setChecked(qPrefTechnicalDetails::hrgraph());
	ui.profRuler->setChecked(qPrefTechnicalDetails::rulergraph());
	ui.profSAC->setChecked(qPrefTechnicalDetails::show_sac());
	ui.profTogglePicture->setChecked(qPrefTechnicalDetails::show_pictures_in_profile());
	ui.profTankbar->setChecked(qPrefTechnicalDetails::tankbar());
	ui.profTissues->setChecked(qPrefTechnicalDetails::percentagegraph());
	ui.profScaled->setChecked(qPrefTechnicalDetails::zoomed_plot());

// full screen support is buggy on Windows and Ubuntu.
// require the FULLSCREEN_SUPPORT macro to enable it!
#ifndef FULLSCREEN_SUPPORT
	ui.actionFullScreen->setEnabled(false);
	ui.actionFullScreen->setVisible(false);
	setWindowState(windowState() & ~Qt::WindowFullScreen);
#endif
}

MainWindow::~MainWindow()
{
	write_hashes();
	m_Instance = nullptr;
}

void MainWindow::setupSocialNetworkMenu()
{
#ifdef FBSUPPORT
	connections = new QMenu(tr("Connect to"));
	FacebookPlugin *facebookPlugin = new FacebookPlugin();
	QAction *toggle_connection = new QAction(this);
	QObject *obj = facebookPlugin;
	toggle_connection->setText(facebookPlugin->socialNetworkName());
	toggle_connection->setIcon(QIcon(facebookPlugin->socialNetworkIcon()));
	toggle_connection->setData(QVariant::fromValue(obj));
	connect(toggle_connection, SIGNAL(triggered()), this, SLOT(socialNetworkRequestConnect()));
	FacebookManager *fb = FacebookManager::instance();
	connect(fb, &FacebookManager::justLoggedIn, this, &MainWindow::facebookLoggedIn);
	connect(fb, &FacebookManager::justLoggedOut, this, &MainWindow::facebookLoggedOut);
	connect(fb, &FacebookManager::sendMessage, [this](const QString& msg) {
		statusBar()->showMessage(msg, 10000); // show message for 10 secs on the statusbar.
	});
	share_on_fb = new QAction(this);
	share_on_fb->setText(facebookPlugin->socialNetworkName());
	share_on_fb->setIcon(QIcon(facebookPlugin->socialNetworkIcon()));
	share_on_fb->setData(QVariant::fromValue(obj));
	share_on_fb->setEnabled(false);
	ui.menuShare_on->addAction(share_on_fb);
	connections->addAction(toggle_connection);
	connect(share_on_fb, SIGNAL(triggered()), this, SLOT(socialNetworkRequestUpload()));
	ui.menuShare_on->addSeparator();
	ui.menuShare_on->addMenu(connections);
	ui.menubar->show();
#endif
}

void MainWindow::facebookLoggedIn()
{
	connections->setTitle(tr("Disconnect from"));
	share_on_fb->setEnabled(true);
}

void MainWindow::facebookLoggedOut()
{
	connections->setTitle(tr("Connect to"));
	share_on_fb->setEnabled(false);
}

void MainWindow::socialNetworkRequestConnect()
{
	qDebug() << "Requesting connect on the social network";
	QAction *action = qobject_cast<QAction*>(sender());
	ISocialNetworkIntegration *plugin = qobject_cast<ISocialNetworkIntegration*>(action->data().value<QObject*>());
	if (plugin->isConnected())
		plugin->requestLogoff();
	else
		plugin->requestLogin();
}

void MainWindow::socialNetworkRequestUpload()
{
	QAction *action = qobject_cast<QAction*>(sender());
	ISocialNetworkIntegration *plugin = action->data().value<ISocialNetworkIntegration*>();
	plugin->requestUpload();
}

void MainWindow::setStateProperties(const QByteArray& state, const PropertyList& tl, const PropertyList& tr, const PropertyList& bl, const PropertyList& br)
{
	stateProperties[state] = PropertiesForQuadrant(tl, tr, bl, br);
}

void MainWindow::on_actionDiveSiteEdit_triggered()
{
	diveSiteEdit->initFields(get_dive_site_for_dive(&displayed_dive));
	setApplicationState("EditDiveSite");
}

void MainWindow::enableDisableCloudActions()
{
	ui.actionCloudstorageopen->setEnabled(prefs.cloud_verification_status == qPrefCloudStorage::CS_VERIFIED);
	ui.actionCloudstoragesave->setEnabled(prefs.cloud_verification_status == qPrefCloudStorage::CS_VERIFIED);
}

void MainWindow::enableDisableOtherDCsActions()
{
	bool nr = number_of_computers(current_dive) > 1;
	ui.actionNextDC->setEnabled(nr);
	ui.actionPreviousDC->setEnabled(nr);
}

void MainWindow::setDefaultState() {
	setApplicationState("Default");
	if (mainTab->getEditMode() != MainTab::NONE) {
		ui.bottomLeft->currentWidget()->setEnabled(false);
	}
}

MainWindow *MainWindow::instance()
{
	return m_Instance;
}

// This gets called after one or more dives were added, edited or downloaded for a dive computer
void MainWindow::refreshDisplay(bool doRecreateDiveList)
{
	mainTab->reload();
	TankInfoModel::instance()->update();
	MapWidget::instance()->reload();
	if (doRecreateDiveList)
		recreateDiveList();

	setApplicationState("Default");
	diveList->setEnabled(true);
	diveList->setFocus();
	WSInfoModel::instance()->updateInfo();
	ui.actionAutoGroup->setChecked(autogroup);
}

void MainWindow::recreateDiveList()
{
	diveList->reload(DiveTripModel::CURRENT);
	TagFilterModel::instance()->repopulate();
	BuddyFilterModel::instance()->repopulate();
	LocationFilterModel::instance()->repopulate();
	SuitsFilterModel::instance()->repopulate();
	MultiFilterSortModel::instance()->myInvalidate();
}

void MainWindow::configureToolbar() {
	if (current_dive) {
		bool freeDiveMode = current_dive->dc.divemode == FREEDIVE;
		ui.profCalcCeiling->setDisabled(freeDiveMode);
		ui.profCalcCeiling->setDisabled(freeDiveMode);
		ui.profCalcAllTissues ->setDisabled(freeDiveMode);
		ui.profIncrement3m->setDisabled(freeDiveMode);
		ui.profDcCeiling->setDisabled(freeDiveMode);
		ui.profPhe->setDisabled(freeDiveMode);
		ui.profPn2->setDisabled(freeDiveMode); //TODO is the same as scuba?
		ui.profPO2->setDisabled(freeDiveMode); //TODO is the same as scuba?
		ui.profTankbar->setDisabled(freeDiveMode);
		ui.profMod->setDisabled(freeDiveMode);
		ui.profNdl_tts->setDisabled(freeDiveMode);
		ui.profEad->setDisabled(freeDiveMode);
		ui.profSAC->setDisabled(freeDiveMode);
		ui.profTissues->setDisabled(freeDiveMode);

		ui.profRuler->setDisabled(false);
		ui.profScaled->setDisabled(false); // measuring and scaling
		ui.profTogglePicture->setDisabled(false);
		ui.profHR->setDisabled(false);
	}
}

void MainWindow::selectionChanged()
{
	if (!current_dive) {
		mainTab->clearTabs();
		mainTab->updateDiveInfo(true);
		graphics->setEmptyState();
	} else {
		graphics->plotDive(nullptr, false, true);
		mainTab->updateDiveInfo();
		configureToolbar();
		enableDisableOtherDCsActions();
		MapWidget::instance()->reload();
	}
}

void MainWindow::on_actionNew_triggered()
{
	on_actionClose_triggered();
}

static QString lastUsedDir()
{
	QString lastDir = QDir::homePath();

	if (QDir(qPrefDisplay::lastDir()).exists())
		lastDir = qPrefDisplay::lastDir();
	return lastDir;
}

void MainWindow::on_actionOpen_triggered()
{
	if (!okToClose(tr("Please save or cancel the current dive edit before opening a new file.")))
		return;

	// yes, this look wrong to use getSaveFileName() for the open dialog, but we need to be able
	// to enter file names that don't exist in order to use our git syntax /path/to/dir[branch]
	// with is a potentially valid input, but of course won't exist. So getOpenFileName() wouldn't work
	QFileDialog dialog(this, tr("Open file"), lastUsedDir(), filter_open());
	dialog.setFileMode(QFileDialog::AnyFile);
	dialog.setViewMode(QFileDialog::Detail);
	dialog.setLabelText(QFileDialog::Accept, tr("Open"));
	dialog.setLabelText(QFileDialog::Reject, tr("Cancel"));
	dialog.setAcceptMode(QFileDialog::AcceptOpen);
	QStringList filenames;
	if (dialog.exec())
		filenames = dialog.selectedFiles();
	if (filenames.isEmpty())
		return;
	updateLastUsedDir(QFileInfo(filenames.first()).dir().path());
	closeCurrentFile();
	// some file dialogs decide to add the default extension to a filename without extension
	// so we would get dir[branch].ssrf when trying to select dir[branch].
	// let's detect that and remove the incorrect extension
	QStringList cleanFilenames;
	QRegularExpression reg(".*\\[[^]]+]\\.ssrf", QRegularExpression::CaseInsensitiveOption);

	Q_FOREACH (QString filename, filenames) {
		if (reg.match(filename).hasMatch())
			filename.remove(QRegularExpression("\\.ssrf$", QRegularExpression::CaseInsensitiveOption));
		cleanFilenames << filename;
	}
	loadFiles(cleanFilenames);
}

void MainWindow::on_actionSave_triggered()
{
	file_save();
}

void MainWindow::on_actionSaveAs_triggered()
{
	file_save_as();
}

void MainWindow::on_actionCloudstorageopen_triggered()
{
	if (!okToClose(tr("Please save or cancel the current dive edit before opening a new file.")))
		return;

	QString filename;
	if (getCloudURL(filename))
		return;

	if (verbose)
		qDebug() << "Opening cloud storage from:" << filename;

	closeCurrentFile();

	showProgressBar();
	QByteArray fileNamePtr = QFile::encodeName(filename);
	if (!parse_file(fileNamePtr.data(), &dive_table))
		setCurrentFile(fileNamePtr.data());
	process_loaded_dives();
	autogroup_dives();
	Command::clear();
	hideProgressBar();
	refreshDisplay();
}

// Return whether saving to cloud is OK. If it isn't, show an error return false.
static bool saveToCloudOK()
{
	if (!dive_table.nr) {
		report_error(qPrintable(gettextFromC::tr("Don't save an empty log to the cloud")));
		return false;
	}
	return true;
}

void MainWindow::on_actionCloudstoragesave_triggered()
{
	QString filename;
	if (!saveToCloudOK())
		return;
	if (getCloudURL(filename))
		return;

	if (verbose)
		qDebug() << "Saving cloud storage to:" << filename;
	if (mainTab->isEditing())
		mainTab->acceptChanges();

	showProgressBar();
	int error = save_dives(qPrintable(filename));
	hideProgressBar();
	if (error)
		return;

	setCurrentFile(qPrintable(filename));
	mark_divelist_changed(false);
}

void MainWindow::on_actionCloudOnline_triggered()
{
	bool isOffline = !ui.actionCloudOnline->isChecked();
	if (isOffline == git_local_only)
		return;

	// Refuse to go online if there is an edit in progress
	if (!isOffline &&
	    (DivePlannerPointsModel::instance()->currentMode() != DivePlannerPointsModel::NOTHING ||
	    mainTab->isEditing())) {
		QMessageBox::warning(this, tr("Warning"), tr("Please save or cancel the current dive edit before going online"));
		// We didn't switch to online, therefore uncheck the checkbox
		ui.actionCloudOnline->setChecked(false);
		return;
	}

	git_local_only = isOffline;
	if (!isOffline) {
		// User requests to go online. Try to sync cloud storage
		if (unsaved_changes()) {
			// If there are unsaved changes, ask the user if they want to save them.
			// If they don't, they have to sync manually.
			if (QMessageBox::warning(this, tr("Save changes?"),
						 tr("You have unsaved changes. Do you want to commit them to the cloud storage?\n"
						    "If answering no, the cloud will only be synced on next call to "
						    "\"Open cloud storage\" or \"Save to cloud storage\"."),
						 QMessageBox::Yes|QMessageBox::No) == QMessageBox::Yes)
				on_actionCloudstoragesave_triggered();
		} else {
			// If there are no unsaved changes, let's just try to load the remote cloud
			on_actionCloudstorageopen_triggered();
		}
		if (git_local_only)
			report_error(qPrintable(tr("Failure taking cloud storage online")));
	}

	setTitle();
	updateCloudOnlineStatus();
}

void MainWindow::cleanUpEmpty()
{
	mainTab->clearTabs();
	mainTab->updateDiveInfo(true);
	graphics->setEmptyState();
	diveList->reload(DiveTripModel::TREE);
	MapWidget::instance()->reload();
	if (!existing_filename)
		setTitle();
	disableShortcuts();
}

bool MainWindow::okToClose(QString message)
{
	if (DivePlannerPointsModel::instance()->currentMode() != DivePlannerPointsModel::NOTHING ||
		mainTab->isEditing() ) {
		QMessageBox::warning(this, tr("Warning"), message);
		return false;
	}
	if (unsaved_changes() && askSaveChanges() == false)
		return false;

	return true;
}

void MainWindow::closeCurrentFile()
{
	graphics->setEmptyState();
	/* free the dives and trips */
	clear_git_id();
	clear_dive_file_data();
	setCurrentFile(nullptr);
	cleanUpEmpty();
	mark_divelist_changed(false);

	clear_events();

	dcList.dcs.clear();
}

void MainWindow::updateCloudOnlineStatus()
{
	bool is_cloud = existing_filename && prefs.cloud_git_url && prefs.cloud_verification_status == qPrefCloudStorage::CS_VERIFIED &&
			strstr(existing_filename, prefs.cloud_git_url);
	ui.actionCloudOnline->setEnabled(is_cloud);
	ui.actionCloudOnline->setChecked(is_cloud && !git_local_only);
}

void MainWindow::setCurrentFile(const char *f)
{
	set_filename(f);
	setTitle();
	updateCloudOnlineStatus();
}

void MainWindow::on_actionClose_triggered()
{
	if (okToClose(tr("Please save or cancel the current dive edit before closing the file."))) {
		closeCurrentFile();
		// hide any pictures and the filter
		DivePictureModel::instance()->updateDivePictures();
		ui.multiFilter->closeFilter();
		recreateDiveList();
	}
}

void MainWindow::updateLastUsedDir(const QString &dir)
{
	qPrefDisplay::set_lastDir(dir);
}

void MainWindow::on_actionPrint_triggered()
{
#ifndef NO_PRINTING
	PrintDialog dlg(this);

	dlg.exec();
#endif
}

void MainWindow::disableShortcuts(bool disablePaste)
{
	ui.actionPreviousDC->setShortcut(QKeySequence());
	ui.actionNextDC->setShortcut(QKeySequence());
	ui.copy->setShortcut(QKeySequence());
	if (disablePaste)
		ui.paste->setShortcut(QKeySequence());
}

void MainWindow::enableShortcuts()
{
	ui.actionPreviousDC->setShortcut(Qt::Key_Left);
	ui.actionNextDC->setShortcut(Qt::Key_Right);
	ui.copy->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_C));
	ui.paste->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_V));
}

void MainWindow::showProfile()
{
	enableShortcuts();
	graphics->setProfileState();
	setApplicationState("Default");
}

void MainWindow::on_actionPreferences_triggered()
{
	// the refreshPages() is currently done for just one
	// reason. Allow the user to define a default cylinder that
	// is not hardcoded but coming from the logbook.
	PreferencesDialog::instance()->refreshPages();

	PreferencesDialog::instance()->show();
	PreferencesDialog::instance()->raise();
}

void MainWindow::on_actionQuit_triggered()
{
	if (mainTab->isEditing()) {
		mainTab->rejectChanges();
		if (mainTab->isEditing())
			// didn't discard the edits
			return;
	}
	if (DivePlannerPointsModel::instance()->currentMode() != DivePlannerPointsModel::NOTHING) {
		DivePlannerPointsModel::instance()->cancelPlan();
		if (DivePlannerPointsModel::instance()->currentMode() != DivePlannerPointsModel::NOTHING)
			// The planned dive was not discarded
			return;
	}

	if (unsaved_changes() && (askSaveChanges() == false))
		return;
	writeSettings();
	QApplication::quit();
}

void MainWindow::on_actionDownloadDC_triggered()
{
	DownloadFromDCWidget dlg(this);
	dlg.exec();
}

void MainWindow::on_actionDivelogs_de_triggered()
{
	DivelogsDeWebServices::instance()->downloadDives();
}

void MainWindow::on_actionEditDeviceNames_triggered()
{
	DiveComputerManagementDialog::instance()->init();
	DiveComputerManagementDialog::instance()->show();
}

bool MainWindow::plannerStateClean()
{
	if (progressDialog)
		// we are accessing the cloud, so let's not switch into Add or Plan mode
		return false;

	if (DivePlannerPointsModel::instance()->currentMode() != DivePlannerPointsModel::NOTHING ||
		mainTab->isEditing()) {
		QMessageBox::warning(this, tr("Warning"), tr("Please save or cancel the current dive edit before trying to add a dive."));
		return false;
	}
	return true;
}

void MainWindow::refreshProfile()
{
	showProfile();
	configureToolbar();
	graphics->replot(current_dive);
	DivePictureModel::instance()->updateDivePictures();
}

void MainWindow::planCanceled()
{
	// while planning we might have modified the displayed_dive
	// let's refresh what's shown on the profile
	refreshProfile();
	refreshDisplay(false);
}

void MainWindow::planCreated()
{
	// make sure our UI is in a consistent state
	showProfile();
	setApplicationState("Default");
	diveList->setEnabled(true);
	diveList->setFocus();
}

void MainWindow::setPlanNotes()
{
	plannerDetails->divePlanOutput()->setHtml(displayed_dive.notes);
}

void MainWindow::updateVariations(QString variations)
{
	QString notes = QString(displayed_dive.notes);
	free(displayed_dive.notes);
	displayed_dive.notes = copy_qstring(notes.replace("VARIATIONS", variations));
	plannerDetails->divePlanOutput()->setHtml(displayed_dive.notes);
}

void MainWindow::printPlan()
{
#ifndef NO_PRINTING
	QString diveplan = plannerDetails->divePlanOutput()->toHtml();
	QString withDisclaimer = QString("<img height=50 src=\":subsurface-icon\"> ") + diveplan + QString(disclaimer);

	QPrinter printer;
	QPrintDialog *dialog = new QPrintDialog(&printer, this);
	dialog->setWindowTitle(tr("Print runtime table"));
	if (dialog->exec() != QDialog::Accepted)
		return;

	/* render the profile as a pixmap that is inserted as base64 data into a HTML <img> tag
	 * make it fit a page width defined by 2 cm margins via QTextDocument->print() (cannot be changed?)
	 * the height of the profile is 40% of the page height.
	 */
	QSizeF renderSize = printer.pageRect(QPrinter::Inch).size();
	const qreal marginsInch = 1.57480315; // = (2 x 2cm) / 2.45cm/inch
	renderSize.setWidth((renderSize.width() - marginsInch) * printer.resolution());
	renderSize.setHeight(((renderSize.height() - marginsInch) * printer.resolution()) / 2.5);

	QPixmap pixmap(renderSize.toSize());
	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setRenderHint(QPainter::SmoothPixmapTransform);

	ProfileWidget2 *profile = graphics;
	QSize origSize = profile->size();
	profile->resize(renderSize.toSize());
	profile->setPrintMode(true);
	profile->render(&painter);
	profile->resize(origSize);
	profile->setPrintMode(false);

	QByteArray byteArray;
	QBuffer buffer(&byteArray);
	pixmap.save(&buffer, "PNG");
	QString profileImage = QString("<img src=\"data:image/png;base64,") + byteArray.toBase64() + "\"/><br><br>";
	withDisclaimer = profileImage + withDisclaimer;

	plannerDetails->divePlanOutput()->setHtml(withDisclaimer);
	plannerDetails->divePlanOutput()->print(&printer);
	plannerDetails->divePlanOutput()->setHtml(displayed_dive.notes);
#endif
}

void MainWindow::setupForAddAndPlan(const char *model)
{
	// clean out the dive and give it an id and the correct dc model
	clear_dive(&displayed_dive);
	displayed_dive.id = dive_getUniqID();
	displayed_dive.when = QDateTime::currentMSecsSinceEpoch() / 1000L + gettimezoneoffset() + 3600;
	displayed_dive.dc.model = strdup(model); // don't translate! this is stored in the XML file
	dc_number = 1;
	// setup the dive cylinders
	DivePlannerPointsModel::instance()->clear();
	DivePlannerPointsModel::instance()->setupCylinders();

}

void MainWindow::on_actionReplanDive_triggered()
{
	if (!plannerStateClean() || !current_dive)
		return;
	else if (!current_dive->dc.model || strcmp(current_dive->dc.model, "planned dive")) {
		if (QMessageBox::warning(this, tr("Warning"), tr("Trying to replan a dive that's not a planned dive."),
					 QMessageBox::Ok | QMessageBox::Cancel) == QMessageBox::Cancel)
					return;
	}
	// put us in PLAN mode
	DivePlannerPointsModel::instance()->clear();
	DivePlannerPointsModel::instance()->setPlanMode(DivePlannerPointsModel::PLAN);

	graphics->setPlanState();
	graphics->clearHandlers();
	setApplicationState("PlanDive");
	divePlannerWidget->setReplanButton(true);
	divePlannerWidget->setupStartTime(QDateTime::fromMSecsSinceEpoch(1000 * current_dive->when, Qt::UTC));
	if (current_dive->surface_pressure.mbar)
		divePlannerWidget->setSurfacePressure(current_dive->surface_pressure.mbar);
	if (current_dive->salinity)
		divePlannerWidget->setSalinity(current_dive->salinity);
	DivePlannerPointsModel::instance()->loadFromDive(current_dive);
	reset_cylinders(&displayed_dive, true);
	CylindersModel::instance()->updateDive();
}

void MainWindow::on_actionDivePlanner_triggered()
{
	if (!plannerStateClean())
		return;

	// put us in PLAN mode
	DivePlannerPointsModel::instance()->setPlanMode(DivePlannerPointsModel::PLAN);
	setApplicationState("PlanDive");

	graphics->setPlanState();

	// create a simple starting dive, using the first gas from the just copied cylinders
	setupForAddAndPlan("planned dive"); // don't translate, stored in XML file
	DivePlannerPointsModel::instance()->setupStartTime();
	DivePlannerPointsModel::instance()->createSimpleDive();
	// plan the dive in the same mode as the currently selected one
	if (current_dive) {
		divePlannerSettingsWidget->setDiveMode(current_dive->dc.divemode);
		if (current_dive->salinity)
			divePlannerWidget->setSalinity(current_dive->salinity);
	}
	DivePictureModel::instance()->updateDivePictures();
	divePlannerWidget->setReplanButton(false);
}

void MainWindow::on_actionAddDive_triggered()
{
	if (!plannerStateClean())
		return;

	if (diveList->selectedTrips().count() >= 1) {
		diveList->rememberSelection();
		diveList->clearSelection();
	}

	setApplicationState("AddDive");
	DivePlannerPointsModel::instance()->setPlanMode(DivePlannerPointsModel::ADD);

	// setup things so we can later create our starting dive
	setupForAddAndPlan("manually added dive"); // don't translate, stored in the XML file

	// now show the mostly empty main tab
	mainTab->updateDiveInfo();

	mainTab->addDiveStarted();

	graphics->setAddState();
	DivePlannerPointsModel::instance()->createSimpleDive();
	configureToolbar();
	graphics->plotDive(nullptr, false, true);
	fixup_dc_duration(&displayed_dive.dc);
	displayed_dive.duration = displayed_dive.dc.duration;

	// now that we have the correct depth and duration, update the dive info
	mainTab->updateDepthDuration();
}

void MainWindow::on_actionRenumber_triggered()
{
	RenumberDialog::instance()->renumberOnlySelected(false);
	RenumberDialog::instance()->show();
}

void MainWindow::on_actionAutoGroup_triggered()
{
	set_autogroup(ui.actionAutoGroup->isChecked());
	if (autogroup)
		Command::autogroupDives();
	else
		Command::removeAutogenTrips();
	refreshDisplay();
	mark_divelist_changed(true);
}

void MainWindow::on_actionYearlyStatistics_triggered()
{
	QDialog d;
	QVBoxLayout *l = new QVBoxLayout(&d);
	YearlyStatisticsModel *m = new YearlyStatisticsModel();
	QTreeView *view = new QTreeView();
	view->setModel(m);
	l->addWidget(view);
	d.resize(lrint(width() * .8), height() / 2);
	d.move(lrint(width() * .1), height() / 4);
	QShortcut *close = new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_W), &d);
	connect(close, SIGNAL(activated()), &d, SLOT(close()));
	QShortcut *quit = new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_Q), &d);
	connect(quit, SIGNAL(activated()), this, SLOT(close()));
	d.setWindowFlags(Qt::Window | Qt::CustomizeWindowHint
		| Qt::WindowCloseButtonHint | Qt::WindowTitleHint);
	d.setWindowTitle(tr("Yearly statistics"));
	d.setWindowIcon(QIcon(":subsurface-icon"));
	d.exec();
}

void MainWindow::toggleCollapsible(bool toggle)
{
	ui.mainSplitter->setCollapsible(0, toggle);
	ui.mainSplitter->setCollapsible(1, toggle);
	ui.topSplitter->setCollapsible(0, toggle);
	ui.topSplitter->setCollapsible(1, toggle);
	ui.bottomSplitter->setCollapsible(0, toggle);
	ui.bottomSplitter->setCollapsible(1, toggle);
}

void MainWindow::on_actionViewList_triggered()
{
	toggleCollapsible(true);
	beginChangeState(LIST_MAXIMIZED);
	ui.mainSplitter->setSizes({ COLLAPSED, EXPANDED });
	ui.bottomSplitter->setSizes({ EXPANDED, COLLAPSED });
}

void MainWindow::on_actionViewProfile_triggered()
{
	toggleCollapsible(true);
	beginChangeState(PROFILE_MAXIMIZED);
	ui.topSplitter->setSizes({ COLLAPSED, EXPANDED });
	ui.mainSplitter->setSizes({ EXPANDED, COLLAPSED });
}

void MainWindow::on_actionViewInfo_triggered()
{
	toggleCollapsible(true);
	beginChangeState(INFO_MAXIMIZED);
	ui.topSplitter->setSizes({ EXPANDED, COLLAPSED });
	ui.mainSplitter->setSizes({ EXPANDED, COLLAPSED });
}

void MainWindow::on_actionViewMap_triggered()
{
	toggleCollapsible(true);
	beginChangeState(MAP_MAXIMIZED);
	ui.mainSplitter->setSizes({ COLLAPSED, EXPANDED });
	ui.bottomSplitter->setSizes({ COLLAPSED, EXPANDED });
}

void MainWindow::on_actionViewAll_triggered()
{
	toggleCollapsible(false);
	beginChangeState(VIEWALL);

	const int appH = qApp->desktop()->size().height();
	const int appW = qApp->desktop()->size().width();

	QList<int> mainSizes = { round_int(appH * 0.7), round_int(appH * 0.3) };
	QList<int> infoProfileSizes = { round_int(appW * 0.3), round_int(appW * 0.7) };
	QList<int> listGlobeSizes = { round_int(appW * 0.7), round_int(appW * 0.3) };

	QSettings settings;
	settings.beginGroup("MainWindow");
	if (settings.value("mainSplitter").isValid()) {
		ui.mainSplitter->restoreState(settings.value("mainSplitter").toByteArray());
		ui.topSplitter->restoreState(settings.value("topSplitter").toByteArray());
		ui.bottomSplitter->restoreState(settings.value("bottomSplitter").toByteArray());
		if (ui.mainSplitter->sizes().first() == 0 || ui.mainSplitter->sizes().last() == 0)
			ui.mainSplitter->setSizes(mainSizes);
		if (ui.topSplitter->sizes().first() == 0 || ui.topSplitter->sizes().last() == 0)
			ui.topSplitter->setSizes(infoProfileSizes);
		if (ui.bottomSplitter->sizes().first() == 0 || ui.bottomSplitter->sizes().last() == 0)
			ui.bottomSplitter->setSizes(listGlobeSizes);

	} else {
		ui.mainSplitter->setSizes(mainSizes);
		ui.topSplitter->setSizes(infoProfileSizes);
		ui.bottomSplitter->setSizes(listGlobeSizes);
	}
	ui.mainSplitter->setCollapsible(0, false);
	ui.mainSplitter->setCollapsible(1, false);
	ui.topSplitter->setCollapsible(0, false);
	ui.topSplitter->setCollapsible(1, false);
	ui.bottomSplitter->setCollapsible(0,false);
	ui.bottomSplitter->setCollapsible(1,false);
}

void MainWindow::enterEditState()
{
	undoAction->setEnabled(false);
	redoAction->setEnabled(false);
	stateBeforeEdit = state;
	if (state == VIEWALL || state == INFO_MAXIMIZED)
		return;
	toggleCollapsible(true);
	beginChangeState(EDIT);
	ui.topSplitter->setSizes({ EXPANDED, EXPANDED });
	ui.mainSplitter->setSizes({ EXPANDED, COLLAPSED });
	int appW = qApp->desktop()->size().width();
	QList<int> infoProfileSizes { round_int(appW * 0.3), round_int(appW * 0.7) };

	QSettings settings;
	settings.beginGroup("MainWindow");
	if (settings.value("mainSplitter").isValid()) {
		ui.topSplitter->restoreState(settings.value("topSplitter").toByteArray());
		if (ui.topSplitter->sizes().first() == 0 || ui.topSplitter->sizes().last() == 0)
			ui.topSplitter->setSizes(infoProfileSizes);
	} else {
		ui.topSplitter->setSizes(infoProfileSizes);
	}
}

void MainWindow::exitEditState()
{
	undoAction->setEnabled(true);
	redoAction->setEnabled(true);
	if (stateBeforeEdit == state)
		return;
	enterState(stateBeforeEdit);
}

void MainWindow::enterState(CurrentState newState)
{
	state = newState;
	switch (state) {
	case VIEWALL:
		on_actionViewAll_triggered();
		break;
	case MAP_MAXIMIZED:
		on_actionViewMap_triggered();
		break;
	case INFO_MAXIMIZED:
		on_actionViewInfo_triggered();
		break;
	case LIST_MAXIMIZED:
		on_actionViewList_triggered();
		break;
	case PROFILE_MAXIMIZED:
		on_actionViewProfile_triggered();
		break;
	case EDIT:
		break;
	}
}

void MainWindow::beginChangeState(CurrentState s)
{
	if (state == VIEWALL && state != s) {
		saveSplitterSizes();
	}
	state = s;
}

void MainWindow::saveSplitterSizes()
{
	QSettings settings;
	settings.beginGroup("MainWindow");
	settings.setValue("mainSplitter", ui.mainSplitter->saveState());
	settings.setValue("topSplitter", ui.topSplitter->saveState());
	settings.setValue("bottomSplitter", ui.bottomSplitter->saveState());
}

void MainWindow::on_actionPreviousDC_triggered()
{
	unsigned nrdc = number_of_computers(current_dive);
	dc_number = (dc_number + nrdc - 1) % nrdc;
	configureToolbar();
	graphics->plotDive(nullptr, false, true);
	mainTab->updateDiveInfo();
}

void MainWindow::on_actionNextDC_triggered()
{
	unsigned nrdc = number_of_computers(current_dive);
	dc_number = (dc_number + 1) % nrdc;
	configureToolbar();
	graphics->plotDive(nullptr, false, true);
	mainTab->updateDiveInfo();
}

void MainWindow::on_actionFullScreen_triggered(bool checked)
{
	if (checked) {
		setWindowState(windowState() | Qt::WindowFullScreen);
	} else {
		setWindowState(windowState() & ~Qt::WindowFullScreen);
	}
}

void MainWindow::on_actionAboutSubsurface_triggered()
{
	SubsurfaceAbout dlg(this);

	dlg.exec();
}

void MainWindow::on_action_Check_for_Updates_triggered()
{
	if (!updateManager)
		updateManager = new UpdateManager(this);

	updateManager->checkForUpdates();
}

void MainWindow::on_actionUserManual_triggered()
{
#ifndef NO_USERMANUAL
	if (!helpView)
		helpView = new UserManual(this);
	helpView->show();
#endif
}

void MainWindow::on_actionUserSurvey_triggered()
{
	if(!survey)
		survey = new UserSurvey(this);
	survey->show();
}

void MainWindow::on_actionHash_images_triggered()
{
	if(!findMovedImagesDialog)
		findMovedImagesDialog = new FindMovedImagesDialog(this);
	findMovedImagesDialog->show();
}

QString MainWindow::filter_open()
{
	QString f = tr("Dive log files") +
		    " (*.ssrf"
		    " *.xml"
		    " *.can"
		    " *.db"
		    " *.sql"
		    " *.dld"
		    " *.jlb"
		    " *.lvd"
		    " *.sde"
		    " *.udcf"
		    " *.uddf"
		    " *.dlf"
		    " *.log"
		    " *.txt"
		    " *.apd"
		    " *.dive"
		    " *.zxu *.zxl"
		    ");;";

	f += tr("Subsurface files") + " (*.ssrf *.xml);;";
	f += tr("Cochran") + " (*.can);;";
	f += tr("DiveLogs.de") + " (*.dld);;";
	f += tr("JDiveLog") + " (*.jlb);;";
	f += tr("Liquivision") + " (*.lvd);;";
	f += tr("Suunto") + " (*.sde *.db);;";
	f += tr("UDCF") + " (*.udcf);;";
	f += tr("UDDF") + " (*.uddf);;";
	f += tr("XML") + " (*.xml);;";
	f += tr("Divesoft") + " (*.dlf);;";
	f += tr("Datatrak/WLog") + " (*.log);;";
	f += tr("MkVI files") + " (*.txt);;";
	f += tr("APD log viewer") + " (*.apd);;";
	f += tr("OSTCtools") + " (*.dive);;";
	f += tr("DAN DL7") + " (*.zxu *.zxl)";

	return f;
}

QString MainWindow::filter_import()
{
	QString f = tr("Dive log files") +
		    " (*.ssrf"
		    " *.xml"
		    " *.can"
		    " *.csv"
		    " *.db"
		    " *.sql"
		    " *.dld"
		    " *.jlb"
		    " *.lvd"
		    " *.sde"
		    " *.udcf"
		    " *.uddf"
		    " *.dlf"
		    " *.log"
		    " *.txt"
		    " *.apd"
		    " *.dive"
		    " *.zxu *.zxl"
		    ");;";

	f += tr("Subsurface files") + " (*.ssrf *.xml);;";
	f += tr("Cochran") + " (*.can);;";
	f += tr("CSV") + " (*.csv *.CSV);;";
	f += tr("DiveLogs.de") + " (*.dld);;";
	f += tr("JDiveLog") + " (*.jlb);;";
	f += tr("Liquivision") + " (*.lvd);;";
	f += tr("Suunto") + " (*.sde *.db);;";
	f += tr("UDCF") + " (*.udcf);;";
	f += tr("UDDF") + " (*.uddf);;";
	f += tr("XML") + " (*.xml);;";
	f += tr("Divesoft") + " (*.dlf);;";
	f += tr("Datatrak/WLog") + " (*.log);;";
	f += tr("MkVI files") + " (*.txt);;";
	f += tr("APD log viewer") + " (*.apd);;";
	f += tr("OSTCtools") + " (*.dive);;";
	f += tr("DAN DL7") + " (*.zxu *.zxl);;";
	f += tr("All files") + " (*.*)";

	return f;
}

bool MainWindow::askSaveChanges()
{
	QMessageBox response(this);

	QString message = existing_filename ?
		tr("Do you want to save the changes that you made in the file %1?").arg(displayedFilename(existing_filename)) :
		tr("Do you want to save the changes that you made in the data file?");

	response.setStandardButtons(QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
	response.setDefaultButton(QMessageBox::Save);
	response.setText(message);
	response.setWindowTitle(tr("Save changes?")); // Not displayed on MacOSX as described in Qt API
	response.setInformativeText(tr("Changes will be lost if you don't save them."));
	response.setIcon(QMessageBox::Warning);
	response.setWindowModality(Qt::WindowModal);
	int ret = response.exec();

	switch (ret) {
	case QMessageBox::Save:
		file_save();
		return true;
	case QMessageBox::Discard:
		return true;
	}
	return false;
}

void MainWindow::initialUiSetup()
{
	QSettings settings;
	settings.beginGroup("MainWindow");
	if (settings.value("maximized", isMaximized()).value<bool>()) {
		showMaximized();
	} else {
		restoreGeometry(settings.value("geometry").toByteArray());
		restoreState(settings.value("windowState", 0).toByteArray());
	}

	enterState((CurrentState)settings.value("lastState", 0).toInt());
	settings.endGroup();
	show();
}

void MainWindow::readSettings()
{
	static bool firstRun = true;
	init_proxy();

	// now make sure that the cloud menu items are enabled IFF cloud account is verified
	enableDisableCloudActions();

	loadRecentFiles();
	if (firstRun) {
		checkSurvey();
		firstRun = false;
	}
}

#undef TOOLBOX_PREF_BUTTON

void MainWindow::checkSurvey()
{
	QSettings s;
	s.beginGroup("UserSurvey");
	if (!s.contains("FirstUse42")) {
		QVariant value = QDate().currentDate();
		s.setValue("FirstUse42", value);
	}
	// wait a week for production versions, but not at all for non-tagged builds
	int waitTime = 7;
	QDate firstUse42 = s.value("FirstUse42").toDate();
	if (run_survey || (firstUse42.daysTo(QDate().currentDate()) > waitTime && !s.contains("SurveyDone"))) {
		if (!survey)
			survey = new UserSurvey(this);
		survey->show();
	}
	s.endGroup();
}

void MainWindow::writeSettings()
{
	QSettings settings;

	settings.beginGroup("MainWindow");
	settings.setValue("geometry", saveGeometry());
	settings.setValue("windowState", saveState());
	settings.setValue("maximized", isMaximized());
	settings.setValue("lastState", (int)state);
	if (state == VIEWALL)
		saveSplitterSizes();
	settings.endGroup();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
	if (DivePlannerPointsModel::instance()->currentMode() != DivePlannerPointsModel::NOTHING ||
		mainTab->isEditing()) {
		on_actionQuit_triggered();
		event->ignore();
		return;
	}

	if (unsaved_changes() && (askSaveChanges() == false)) {
		event->ignore();
		return;
	}
	event->accept();
	writeSettings();
	QApplication::closeAllWindows();
}

void MainWindow::loadRecentFiles()
{
	recentFiles.clear();
	QSettings s;
	s.beginGroup("Recent_Files");
	foreach (const QString &key, s.childKeys()) {
		// TODO Sorting only correct up to 9 entries. Currently, only 4 used, so no problem.
		if (!key.startsWith("File_"))
			continue;
		QString file = s.value(key).toString();
		if (QFile::exists(file))
			recentFiles.append(file);
		if (recentFiles.count() > NUM_RECENT_FILES)
			break;
	}
	s.endGroup();
	updateRecentFilesMenu();
}

void MainWindow::updateRecentFilesMenu()
{
	for (int c = 0; c < NUM_RECENT_FILES; c++) {
		QAction *action = actionsRecent[c];

		if (recentFiles.count() > c) {
			QFileInfo fi(recentFiles.at(c));
			action->setText(fi.fileName());
			action->setToolTip(fi.absoluteFilePath());
			action->setVisible(true);
		} else {
			action->setVisible(false);
		}
	}
}

void MainWindow::addRecentFile(const QString &file, bool update)
{
	QString localFile = QDir::toNativeSeparators(file);
	int index = recentFiles.indexOf(localFile);
	if (index >= 0)
		recentFiles.removeAt(index);
	recentFiles.prepend(localFile);
	while (recentFiles.count() > NUM_RECENT_FILES)
		recentFiles.removeLast();
	if (update)
		updateRecentFiles();
}

void MainWindow::updateRecentFiles()
{
	QSettings s;

	s.beginGroup("Recent_Files");
	s.remove("");	// Remove all old entries
	for (int c = 1; c <= recentFiles.count(); c++) {
		QString key = QString("File_%1").arg(c);
		s.setValue(key, recentFiles.at(c - 1));
	}
	s.endGroup();
	s.sync();
	updateRecentFilesMenu();
}

void MainWindow::recentFileTriggered(bool)
{
	if (!okToClose(tr("Please save or cancel the current dive edit before opening a new file.")))
		return;

	int filenr = ((QAction *)sender())->data().toInt();
	if (filenr >= recentFiles.count())
		return;
	const QString &filename = recentFiles[filenr];

	updateLastUsedDir(QFileInfo(filename).dir().path());
	closeCurrentFile();
	loadFiles(QStringList() << filename);
}

int MainWindow::file_save_as(void)
{
	QString filename;
	const char *default_filename = existing_filename;

	// if the default is to save to cloud storage, pick something that will work as local file:
	// simply extract the branch name which should be the users email address
	if (default_filename && strstr(default_filename, prefs.cloud_git_url)) {
		QString filename(default_filename);
		filename.remove(prefs.cloud_git_url);
		filename.remove(0, filename.indexOf("[") + 1);
		filename.replace("]", ".ssrf");
		default_filename = copy_qstring(filename);
	}
	// create a file dialog that allows us to save to a new file
	QFileDialog selection_dialog(this, tr("Save file as"), default_filename,
					 tr("Subsurface files") + " (*.ssrf *.xml)");
	selection_dialog.setAcceptMode(QFileDialog::AcceptSave);
	selection_dialog.setFileMode(QFileDialog::AnyFile);
	selection_dialog.setDefaultSuffix("");
	if (empty_string(default_filename)) {
		QFileInfo defaultFile(system_default_filename());
		selection_dialog.setDirectory(qPrintable(defaultFile.absolutePath()));
	}
	/* if the exit/cancel button is pressed return */
	if (!selection_dialog.exec())
		return 0;

	/* get the first selected file */
	filename = selection_dialog.selectedFiles().at(0);

	/* now for reasons I don't understand we appear to add a .ssrf to
	 * git style filenames <path>/directory[branch]
	 * so let's remove that */
	QRegularExpression reg(".*\\[[^]]+]\\.ssrf", QRegularExpression::CaseInsensitiveOption);
	if (reg.match(filename).hasMatch())
		filename.remove(QRegularExpression("\\.ssrf$", QRegularExpression::CaseInsensitiveOption));
	if (filename.isNull() || filename.isEmpty())
		return report_error("No filename to save into");

	if (mainTab->isEditing())
		mainTab->acceptChanges();

	if (save_dives(qPrintable(filename)))
		return -1;

	setCurrentFile(qPrintable(filename));
	mark_divelist_changed(false);
	addRecentFile(filename, true);
	return 0;
}

int MainWindow::file_save(void)
{
	const char *current_default;
	bool is_cloud = false;

	if (!existing_filename)
		return file_save_as();

	is_cloud = (strncmp(existing_filename, "http", 4) == 0);
	if (is_cloud && !saveToCloudOK())
		return -1;

	if (mainTab->isEditing())
		mainTab->acceptChanges();

	current_default = prefs.default_filename;
	if (strcmp(existing_filename, current_default) == 0) {
		/* if we are using the default filename the directory
		 * that we are creating the file in may not exist */
		QDir current_def_dir = QFileInfo(current_default).absoluteDir();
		if (!current_def_dir.exists())
			current_def_dir.mkpath(current_def_dir.absolutePath());
	}
	if (is_cloud)
		showProgressBar();
	if (save_dives(existing_filename)) {
		if (is_cloud)
			hideProgressBar();
		return -1;
	}
	if (is_cloud)
		hideProgressBar();
	mark_divelist_changed(false);
	addRecentFile(QString(existing_filename), true);
	return 0;
}

NotificationWidget *MainWindow::getNotificationWidget()
{
	return ui.mainErrorMessage;
}

QString MainWindow::displayedFilename(QString fullFilename)
{
	QFile f(fullFilename);
	QFileInfo fileInfo(f);
	QString fileName(fileInfo.fileName());

	if (fullFilename.contains(prefs.cloud_git_url)) {
		QString email = fileName.left(fileName.indexOf('['));
		return git_local_only ?
			tr("[local cache for] %1").arg(email) :
			tr("[cloud storage for] %1").arg(email);
	} else {
		return fileName;
	}
}


void MainWindow::setAutomaticTitle()
{
	setTitle();
}

void MainWindow::setTitle()
{
	if (empty_string(existing_filename)) {
		setWindowTitle("Subsurface");
		return;
	}

	QString unsaved = (unsaved_changes() ? " *" : "");
	setWindowTitle("Subsurface: " + displayedFilename(existing_filename) + unsaved);
}

void MainWindow::importFiles(const QStringList fileNames)
{
	if (fileNames.isEmpty())
		return;

	QByteArray fileNamePtr;
	struct dive_table table = { 0 };

	for (int i = 0; i < fileNames.size(); ++i) {
		fileNamePtr = QFile::encodeName(fileNames.at(i));
		parse_file(fileNamePtr.data(), &table);
	}
	process_imported_dives(&table, false, false);
	autogroup_dives();
	Command::clear();
	refreshDisplay();
}

void MainWindow::loadFiles(const QStringList fileNames)
{
	if (fileNames.isEmpty()) {
		refreshDisplay();
		return;
	}
	QByteArray fileNamePtr;

	showProgressBar();
	for (int i = 0; i < fileNames.size(); ++i) {
		fileNamePtr = QFile::encodeName(fileNames.at(i));
		if (!parse_file(fileNamePtr.data(), &dive_table)) {
			setCurrentFile(fileNamePtr.data());
			addRecentFile(fileNamePtr, false);
		}
	}
	hideProgressBar();
	updateRecentFiles();
	process_loaded_dives();
	autogroup_dives();
	Command::clear();

	refreshDisplay();

	int min_datafile_version = get_min_datafile_version();
	if (min_datafile_version >0 && min_datafile_version < DATAFORMAT_VERSION) {
		QMessageBox::warning(this, tr("Opening datafile from older version"),
				     tr("You opened a data file from an older version of Subsurface. We recommend "
					"you read the manual to learn about the changes in the new version, especially "
					"about dive site management which has changed significantly.\n"
					"Subsurface has already tried to pre-populate the data but it might be worth "
					"while taking a look at the new dive site management system and to make "
					"sure that everything looks correct."));
	}
}

static const char *csvExtensions[] = {
	".csv", ".apd", ".zxu", ".zxl", ".txt"
};

static bool isCsvFile(const QString &s)
{
	for (const char *ext: csvExtensions) {
		if (s.endsWith(ext, Qt::CaseInsensitive))
			return true;
	}
	return false;
}

void MainWindow::on_actionImportDiveLog_triggered()
{
	QStringList fileNames = QFileDialog::getOpenFileNames(this, tr("Open dive log file"), lastUsedDir(), filter_import());

	if (fileNames.isEmpty())
		return;
	updateLastUsedDir(QFileInfo(fileNames[0]).dir().path());

	QStringList logFiles;
	QStringList csvFiles;
	for (const QString &fn: fileNames) {
		if (isCsvFile(fn))
			csvFiles.append(fn);
		else
			logFiles.append(fn);
	}

	if (logFiles.size()) {
		importFiles(logFiles);
	}

	if (csvFiles.size()) {
		DiveLogImportDialog diveLogImport(csvFiles, this);
		diveLogImport.exec();
	}
}

void MainWindow::editCurrentDive()
{
	if (!current_dive)
		return;

	if (mainTab->isEditing() || DivePlannerPointsModel::instance()->currentMode() != DivePlannerPointsModel::NOTHING) {
		QMessageBox::warning(this, tr("Warning"), tr("Please, first finish the current edition before trying to do another."));
		return;
	}

	struct dive *d = current_dive;
	QString defaultDC(d->dc.model);
	DivePlannerPointsModel::instance()->clear();
	disableShortcuts();
	if (defaultDC == "manually added dive") {
		DivePlannerPointsModel::instance()->setPlanMode(DivePlannerPointsModel::ADD);
		graphics->setAddState();
		setApplicationState("EditDive");
		DivePlannerPointsModel::instance()->loadFromDive(d);
		mainTab->enableEdition(MainTab::MANUALLY_ADDED_DIVE);
	} else if (defaultDC == "planned dive") {
		DivePlannerPointsModel::instance()->setPlanMode(DivePlannerPointsModel::PLAN);
		setApplicationState("EditPlannedDive");
		DivePlannerPointsModel::instance()->loadFromDive(d);
		mainTab->enableEdition(MainTab::MANUALLY_ADDED_DIVE);
	} else {
		setApplicationState("EditDive");
		mainTab->enableEdition();
	}
}

void MainWindow::turnOffNdlTts()
{
	qPrefTechnicalDetails::set_calcndltts(false);
}

#undef TOOLBOX_PREF_PROFILE
#undef PERF_PROFILE

void MainWindow::on_actionExport_triggered()
{
	DiveLogExportDialog diveLogExport;
	diveLogExport.exec();
}

void MainWindow::on_actionConfigure_Dive_Computer_triggered()
{
	ConfigureDiveComputerDialog *dcConfig = new ConfigureDiveComputerDialog(this);
	dcConfig->show();
}

void MainWindow::setEnabledToolbar(bool arg1)
{
	Q_FOREACH (QAction *b, profileToolbarActions)
		b->setEnabled(arg1);
}

void MainWindow::on_copy_triggered()
{
	// open dialog to select what gets copied
	// copy the displayed dive
	DiveComponentSelection dialog(this, &copyPasteDive, &what);
	dialog.exec();
}

void MainWindow::on_paste_triggered()
{
	// take the data in our copyPasteDive and apply it to selected dives
	selective_copy_dive(&copyPasteDive, &displayed_dive, what, false);
	mainTab->showAndTriggerEditSelective(what);
}

void MainWindow::on_actionFilterTags_triggered()
{
	if (ui.multiFilter->isVisible()) {
		ui.multiFilter->closeFilter();
		ui.actionFilterTags->setChecked(false);
	} else {
		ui.multiFilter->setVisible(true);
		ui.actionFilterTags->setChecked(true);
	}
}

void MainWindow::setCheckedActionFilterTags(bool checked)
{
	ui.actionFilterTags->setChecked(checked);
}

void MainWindow::registerApplicationState(const QByteArray& state, QWidget *topLeft, QWidget *topRight, QWidget *bottomLeft, QWidget *bottomRight)
{
	applicationState[state] = WidgetForQuadrant(topLeft, topRight, bottomLeft, bottomRight);
	if (ui.topLeft->indexOf(topLeft) == -1 && topLeft) {
		ui.topLeft->addWidget(topLeft);
	}
	if (ui.topRight->indexOf(topRight) == -1 && topRight) {
		ui.topRight->addWidget(topRight);
	}
	if (ui.bottomLeft->indexOf(bottomLeft) == -1 && bottomLeft) {
		ui.bottomLeft->addWidget(bottomLeft);
	}
	if(ui.bottomRight->indexOf(bottomRight) == -1 && bottomRight) {
		ui.bottomRight->addWidget(bottomRight);
	}
}

void MainWindow::setApplicationState(const QByteArray& state) {
	if (!applicationState.keys().contains(state))
		return;

	if (getCurrentAppState() == state)
		return;

	setCurrentAppState(state);

#define SET_CURRENT_INDEX( X ) \
	if (applicationState[state].X) { \
		ui.X->setCurrentWidget( applicationState[state].X); \
		ui.X->show(); \
	} else { \
		ui.X->hide(); \
	}

	SET_CURRENT_INDEX( topLeft )
	Q_FOREACH(const WidgetProperty& p, stateProperties[state].topLeft) {
		ui.topLeft->currentWidget()->setProperty( p.first.data(), p.second);
	}
	SET_CURRENT_INDEX( topRight )
	Q_FOREACH(const WidgetProperty& p, stateProperties[state].topRight) {
		ui.topRight->currentWidget()->setProperty( p.first.data(), p.second);
	}
	SET_CURRENT_INDEX( bottomLeft )
	Q_FOREACH(const WidgetProperty& p, stateProperties[state].bottomLeft) {
		ui.bottomLeft->currentWidget()->setProperty( p.first.data(), p.second);
	}
	SET_CURRENT_INDEX( bottomRight )
	Q_FOREACH(const WidgetProperty& p, stateProperties[state].bottomRight) {
		ui.bottomRight->currentWidget()->setProperty( p.first.data(), p.second);
	}
#undef SET_CURRENT_INDEX
}

void MainWindow::showProgressBar()
{
	delete progressDialog;

	progressDialog = new QProgressDialog(tr("Contacting cloud service..."), tr("Cancel"), 0, 100, this);
	progressDialog->setWindowModality(Qt::WindowModal);
	progressDialog->setMinimumDuration(0);
	progressDialogCanceled = false;
	progressCounter = 0;
	connect(progressDialog, SIGNAL(canceled()), this, SLOT(cancelCloudStorageOperation()));
}

void MainWindow::cancelCloudStorageOperation()
{
	progressDialogCanceled = true;
}

void MainWindow::hideProgressBar()
{
	if (progressDialog) {
		progressDialog->setValue(100);
		delete progressDialog;
		progressDialog = nullptr;
	}
}

void MainWindow::unsetProfHR()
{
	ui.profHR->setChecked(false);
	qPrefTechnicalDetails::set_hrgraph(false);
}

void MainWindow::unsetProfTissues()
{
	ui.profTissues->setChecked(false);
	qPrefTechnicalDetails::set_percentagegraph(false);
}
