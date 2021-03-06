/*

    Copyright (c) 2015 Oliver Lau <ola@ct.de>, Heise Medien GmbH & Co. KG

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/


#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QDebug>
#include <QLibraryInfo>
#include <QTranslator>
#include <QLocale>
#include <QObject>
#include <QList>
#include <QPair>
#include <QClipboard>
#include <QStringListModel>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkSession>
#include <QSslCipher>
#include <QSslCertificate>
#include <QSslCertificateExtension>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QSslError>
#include <QSslKey>
#include <QUrlQuery>
#include <QProgressDialog>
#include <QSysInfo>
#include <QElapsedTimer>
#include <QtConcurrent>
#include <QFuture>
#include <QFutureWatcher>
#include <QMutexLocker>
#include <QSemaphore>
#include <QDesktopServices>
#include <QCompleter>
#include <QShortcut>
#include <QGraphicsOpacityEffect>
#include <QLockFile>
#include <QPainter>
#include <QPixmap>
#include <QCursor>

#include "logger.h"
#include "global.h"
#include "util.h"
#include "progressdialog.h"
#include "masterpassworddialog.h"
#include "changemasterpassworddialog.h"
#include "optionsdialog.h"
#include "easyselectorwidget.h"
#include "countdownwidget.h"
#include "expandablegroupbox.h"
#if HACKING_MODE_ENABLED
#include "hackhelper.h"
#endif
#include "pbkdf2.h"
#include "password.h"
#include "crypter.h"
#include "securebytearray.h"
#include "securestring.h"
#include "passwordchecker.h"
#include "tcpclient.h"
#include "exporter.h"
#include "keepass2xmlreader.h"
#include "passwordsafereader.h"

#include "qrencode.h"

static const int DefaultMasterPasswordInvalidationTimeMins = 5;
static const bool CompressionEnabled = true;
static const int NotFound = -1;

enum TabIndexes {
  TabGeneratedPassword,
  TabLegacyPassword,
  TabAttachments
};

static const QString DefaultSyncServerRoot = "https://syncserver.net/ctSESAM";
static const QString DefaultSyncServerUsername = "inter";
static const QString DefaultSyncServerPassword = "op";
static const QString DefaultSyncServerWriteUrl = "/ajax/write.php";
static const QString DefaultSyncServerReadUrl = "/ajax/read.php";
static const QString DefaultSyncServerDeleteUrl = "/ajax/delete.php";

const int MainWindow::EXIT_CODE_RESTART_APP = -12345679;


class MainWindowPrivate {
public:
  explicit MainWindowPrivate(QWidget *parent)
    : langGroup(Q_NULLPTR)
    , masterPasswordDialog(new MasterPasswordDialog(parent))
    , changeMasterPasswordDialog(new ChangeMasterPasswordDialog(parent))
    , optionsDialog(new OptionsDialog(parent))
    , progressDialog(new ProgressDialog(parent))
    , countdownWidget(new CountdownWidget)
    , trayMenu(Q_NULLPTR)
    , actionShow(Q_NULLPTR)
    , actionLockApplication(Q_NULLPTR)
    , attachmentsContextMenu(Q_NULLPTR)
    , actionSaveAttachment(Q_NULLPTR)
    , actionDeleteAttachment(Q_NULLPTR)
    , actionAttachFile(Q_NULLPTR)
    , settings(QSettings::IniFormat, QSettings::UserScope, AppCompanyName, AppName)
    , customCharacterSetDirty(false)
    , parameterSetDirty(false)
    , expandableGroupBox(new ExpandableGroupbox)
    , expandableGroupBoxLastExpanded(false)
#if HACKING_MODE_ENABLED
    , hackIterationDurationMs(0)
    , hackSalt(4, 0)
    , hackPermutations(1)
    , hackingMode(false)
#endif
    , trayIcon(QIcon(":/images/ctSESAM.ico"))
    , salt(Crypter::generateSalt())
    , deleteReply(Q_NULLPTR)
    , readReply(Q_NULLPTR)
    , writeReply(Q_NULLPTR)
    , completer(Q_NULLPTR)
    , pwdLabelOpacityEffect(Q_NULLPTR)
    , counter(0)
    , maxCounter(0)
    , masterPasswordChangeStep(0)
    , interactionSemaphore(1)
    , doConvertLocalToLegacy(false)
    , lockFile(Q_NULLPTR)
    , forceStart(false)
  {
    resetSSLConf();
  }
  ~MainWindowPrivate()
  {
    SecureErase(masterPassword);
  }
  void resetSSLConf(void)
  {
    sslConf = QSslConfiguration::defaultConfiguration();
    sslConf.setCiphers(QSslSocket::supportedCiphers());
  }
  const SecureByteArray &kgk(void) {
    if (KGK.isEmpty()) {
      KGK = Crypter::generateKGK();
    }
    return KGK;
  }
  QString language;
  QActionGroup *langGroup;
  MasterPasswordDialog *masterPasswordDialog;
  ChangeMasterPasswordDialog *changeMasterPasswordDialog;
  OptionsDialog *optionsDialog;
  ProgressDialog *progressDialog;
  CountdownWidget *countdownWidget;
  QMenu *trayMenu;
  QAction *actionShow;
  QAction *actionLockApplication;
  QMenu *attachmentsContextMenu;
  QAction *actionSaveAttachment;
  QAction *actionDeleteAttachment;
  QAction *actionAttachFile;
  QString lastDomainBeforeLock;
  DomainSettings lastCleanDomainSettings;
  DomainSettings domainSettingsBeforceSync;
  QSettings settings;
  DomainSettingsList domains;
  DomainSettingsList remoteDomains;
  bool customCharacterSetDirty;
  bool parameterSetDirty;
  ExpandableGroupbox *expandableGroupBox;
  bool expandableGroupBoxLastExpanded;
#if HACKING_MODE_ENABLED
  qint64 hackIterationDurationMs;
  QElapsedTimer hackClock;
  QElapsedTimer hackIterationClock;
  QByteArray hackSalt;
  PositionTable hackPos;
  qint64 hackPermutations;
  bool hackingMode;
#endif
  Password password;
  QDateTime createdDate;
  QDateTime modifiedDate;
  QSystemTrayIcon trayIcon;
  QByteArray salt;
  SecureByteArray masterKey;
  SecureByteArray IV;
  SecureByteArray KGK;
  QFuture<void> keyGenerationFuture;
  QMutex keyGenerationMutex;
  QString masterPassword;
  QSslConfiguration sslConf;
  QNetworkAccessManager deleteNAM;
  QNetworkAccessManager readNAM;
  QNetworkAccessManager writeNAM;
  QNetworkReply *deleteReply;
  QNetworkReply *readReply;
  QNetworkReply *writeReply;
  QCompleter *completer;
  QGraphicsOpacityEffect *pwdLabelOpacityEffect;
  int counter;
  int maxCounter;
  int masterPasswordChangeStep;
  QSemaphore interactionSemaphore;
  QFuture<void> backupFileDeletionFuture;
  TcpClient tcpClient;
  bool doConvertLocalToLegacy;
  QLockFile *lockFile;
  bool forceStart;
  QString lastAttachFileDir;
  QString lastSaveAttachmentDir;
};


MainWindow::MainWindow(bool forceStart, QWidget *parent)
  : QMainWindow(parent)
  , ui(new Ui::MainWindow)
  , d_ptr(new MainWindowPrivate(this))
{
  Q_D(MainWindow);

  // Logger::instance().setFileName(QString("%1/%2.log").arg(QStandardPaths::writableLocation(QStandardPaths::DataLocation)).arg(AppName));
  _LOG("MainWindow::MainWindow()");
  d->forceStart = forceStart;
  const QString lockfilePath = QDir::homePath() + "/.qt-sesam.lck";
  d->lockFile = new QLockFile(lockfilePath);
  if (!d->lockFile->tryLock()) {
    _LOG(QString("Lock file detected at %1").arg(lockfilePath));
    if (!d->forceStart) {
      qint64 appId;
      QString hostName;
      QString appName;
      d->lockFile->getLockInfo(&appId, &hostName, &appName);
      if (isRunning(appId)) {
        QMessageBox::information(this,
                                 tr("%1 cannot run concurrently").arg(AppName),
                                 tr("Only one instance of %1 can run at a time. "
                                    "Another instance is running with process ID %2. "
                                    "Please stop that process before starting a new one.")
                                 .arg(AppName)
                                 .arg(appId));
        close();
        ::exit(1);
      }
      else {
        QMessageBox::StandardButton button =
            QMessageBox::question(this,
                                  tr("%1 cannot run concurrently").arg(AppName),
                                  tr("Only one instance of %1 can run at a time. "
                                     "But a lock file is present in %2 telling "
                                     "that currently there's another instance running with process ID %3. "
                                     "Do you want to override this lock? "
                                     "Please only answer with YES if really no other instance is running at the moment. "
                                     "This might be the case if the system crashed leaving an stale lock file behind.")
                                  .arg(AppName)
                                  .arg(lockfilePath)
                                  .arg(appId),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (button == QMessageBox::Yes) {
          d->lockFile->removeStaleLockFile();
          _LOG("Removing stale lock file");
        }
        else {
          close();
          ::exit(1);
        }
      }
    }
    else {
      d->lockFile->removeStaleLockFile();
      _LOG("Removing stale lock file");
    }
  }

  ui->setupUi(this);
  setWindowIcon(QIcon(":/images/ctSESAM.ico"));
  restoreUiSettings();

  ui->selectorGridLayout->addWidget(ui->easySelectorWidget, 0, 1);
  QObject::connect(ui->easySelectorWidget, SIGNAL(valuesChanged(int, int)), SLOT(onEasySelectorValuesChanged(int, int)));
  QObject::connect(d->optionsDialog, SIGNAL(maxPasswordLengthChanged(int)), ui->easySelectorWidget, SLOT(setMaxLength(int)));
  QObject::connect(d->optionsDialog, SIGNAL(masterPasswordInvalidationTimeMinsChanged(int)), SLOT(masterPasswordInvalidationTimeMinsChanged(int)));
  QObject::connect(this, SIGNAL(backupFilesDeleted(bool)), SLOT(onBackupFilesRemoved(bool)));
  QObject::connect(this, SIGNAL(backupFilesDeleted(int)), SLOT(onBackupFilesRemoved(int)));
  resetAllFields();

  QObject::connect(ui->domainsComboBox, SIGNAL(editTextChanged(QString)), SLOT(onDomainTextChanged(QString)));
  QObject::connect(ui->domainsComboBox, SIGNAL(currentIndexChanged(QString)), SLOT(onDomainSelected(QString)));
  ui->domainsComboBox->installEventFilter(this);
  QObject::connect(ui->userLineEdit, SIGNAL(textChanged(QString)), SLOT(onUserChanged(QString)));
  ui->userLineEdit->installEventFilter(this);
  QObject::connect(ui->urlLineEdit, SIGNAL(textChanged(QString)), SLOT(onURLChanged(QString)));
  ui->urlLineEdit->installEventFilter(this);
  QObject::connect(ui->openURLPushButton, SIGNAL(pressed()), SLOT(openURL()));
  QObject::connect(ui->legacyPasswordLineEdit, SIGNAL(textEdited(QString)), SLOT(onLegacyPasswordChanged(QString)));
  ui->legacyPasswordLineEdit->installEventFilter(this);
  QObject::connect(ui->notesPlainTextEdit, SIGNAL(textChanged()), SLOT(onNotesChanged()));
  ui->notesPlainTextEdit->installEventFilter(this);
  QObject::connect(ui->extraLineEdit, SIGNAL(textChanged(QString)), SLOT(onExtraCharactersChanged(QString)));
  QObject::connect(ui->deleteCheckBox, SIGNAL(toggled(bool)), SLOT(onDeleteChanged(bool)));
  QObject::connect(ui->iterationsSpinBox, SIGNAL(valueChanged(int)), SLOT(onIterationsChanged(int)));
  QObject::connect(ui->saltBase64LineEdit, SIGNAL(textChanged(QString)), SLOT(onSaltChanged(QString)));
  ui->generatedPasswordLineEdit->installEventFilter(this);
  QObject::connect(ui->passwordTemplateLineEdit, SIGNAL(textChanged(QString)), SLOT(onPasswordTemplateChanged(QString)));
  QObject::connect(ui->copyGeneratedPasswordToClipboardPushButton, SIGNAL(clicked()), SLOT(copyGeneratedPasswordToClipboard()));
  QObject::connect(ui->copyLegacyPasswordToClipboardPushButton, SIGNAL(clicked()), SLOT(copyLegacyPasswordToClipboard()));
  QObject::connect(ui->copyUsernameToClipboardPushButton, SIGNAL(clicked()), SLOT(copyUsernameToClipboard()));
  QObject::connect(ui->renewSaltPushButton, SIGNAL(clicked()), SLOT(onRenewSalt()));
  QObject::connect(ui->revertPushButton, SIGNAL(clicked(bool)), SLOT(onRevert()));
  QObject::connect(ui->savePushButton, SIGNAL(clicked(bool)), SLOT(saveCurrentDomainSettings()));
  QObject::connect(ui->loginPushButton, SIGNAL(clicked(bool)), SLOT(onLogin()));
  QObject::connect(ui->tabWidget, SIGNAL(currentChanged(int)), SLOT(onTabChanged(int)));
  QObject::connect(ui->shuffleUsernamePushButton, SIGNAL(clicked(bool)), SLOT(onShuffleUsername()));
  QObject::connect(ui->actionNewDomain, SIGNAL(triggered(bool)), SLOT(onNewDomain()));
  QObject::connect(ui->actionSave, SIGNAL(triggered(bool)), SLOT(saveCurrentDomainSettings()));
  QObject::connect(ui->actionClearAllSettings, SIGNAL(triggered(bool)), SLOT(clearAllSettings()));
  QObject::connect(ui->actionSyncNow, SIGNAL(triggered(bool)), SLOT(onSync()));
  QObject::connect(ui->actionForcedPush, SIGNAL(triggered(bool)), SLOT(onForcedPush()));
  QObject::connect(ui->actionLockApplication, SIGNAL(triggered(bool)), SLOT(lockApplication()));
  QObject::connect(ui->actionClearClipboard, SIGNAL(triggered(bool)), SLOT(clearClipboard()));
  QObject::connect(ui->actionExit, SIGNAL(triggered(bool)), SLOT(close()));
  QObject::connect(ui->actionAbout, SIGNAL(triggered(bool)), SLOT(about()));
  QObject::connect(ui->actionAboutQt, SIGNAL(triggered(bool)), SLOT(aboutQt()));
  QObject::connect(ui->actionOptions, SIGNAL(triggered(bool)), SLOT(showOptionsDialog()));
  QObject::connect(ui->actionExportAllDomainSettingsAsJSON, SIGNAL(triggered(bool)), SLOT(onExportAllDomainSettingAsJSON()));
  QObject::connect(ui->actionExportAllLoginDataAsClearText, SIGNAL(triggered(bool)), SLOT(onExportAllLoginDataAsClearText()));
  QObject::connect(ui->actionExportCurrentSettingsAsQRCode, SIGNAL(triggered(bool)), SLOT(onExportCurrentSettingsAsQRCode()));
  QObject::connect(ui->actionExportKGK, SIGNAL(triggered(bool)), SLOT(onExportKGK()));
  QObject::connect(ui->actionImportKGK, SIGNAL(triggered(bool)), SLOT(onImportKGK()));
  QObject::connect(ui->actionKeePassXmlFile, SIGNAL(triggered(bool)), SLOT(onImportKeePass2XmlFile()));
  QObject::connect(ui->actionPasswordSafeFile, SIGNAL(triggered(bool)), SLOT(onImportPasswordSafeFile()));
  QObject::connect(d->optionsDialog, SIGNAL(serverCertificatesUpdated(QList<QSslCertificate>)), SLOT(onServerCertificatesUpdated(QList<QSslCertificate>)));
  QObject::connect(d->masterPasswordDialog, SIGNAL(accepted()), SLOT(onMasterPasswordEntered()));
  QObject::connect(d->masterPasswordDialog, SIGNAL(closing()), SLOT(onMasterPasswordClosing()), Qt::DirectConnection);
  QObject::connect(d->countdownWidget, SIGNAL(timeout()), SLOT(lockApplication()));
  QObject::connect(ui->actionChangeMasterPassword, SIGNAL(triggered(bool)), SLOT(changeMasterPassword()));
  QObject::connect(ui->actionDeleteOldBackupFiles, SIGNAL(triggered(bool)), SLOT(removeOutdatedBackupFiles()));
#if HACKING_MODE_ENABLED
  QObject::connect(ui->actionHackLegacyPassword, SIGNAL(triggered(bool)), SLOT(hackLegacyPassword()));
#else
  ui->actionHackLegacyPassword->setVisible(false);
#endif
  QObject::connect(ui->actionRegenerateSaltKeyIV, SIGNAL(triggered(bool)), SLOT(generateSaltKeyIV()));
  QObject::connect(this, SIGNAL(saltKeyIVGenerated()), SLOT(onGeneratedSaltKeyIV()), Qt::ConnectionType::QueuedConnection);
  QObject::connect(d->progressDialog, SIGNAL(cancelled()), SLOT(cancelServerOperation()));

  QObject::connect(&d->password, SIGNAL(generated()), SLOT(onPasswordGenerated()));
  QObject::connect(&d->password, SIGNAL(generationAborted()), SLOT(onPasswordGenerationAborted()));
  QObject::connect(&d->password, SIGNAL(generationStarted()), SLOT(onPasswordGenerationStarted()));

  QObject::connect(&d->tcpClient, SIGNAL(receivedMessage(QJsonDocument)), SLOT(onMessageFromTcpClient(QJsonDocument)));

  QObject::connect(&d->deleteNAM, SIGNAL(finished(QNetworkReply*)), SLOT(onDeleteFinished(QNetworkReply*)));
  QObject::connect(&d->deleteNAM, SIGNAL(sslErrors(QNetworkReply*,QList<QSslError>)), SLOT(sslErrorsOccured(QNetworkReply*,QList<QSslError>)));
  QObject::connect(&d->readNAM, SIGNAL(finished(QNetworkReply*)), SLOT(onReadFinished(QNetworkReply*)));
  QObject::connect(&d->readNAM, SIGNAL(sslErrors(QNetworkReply*,QList<QSslError>)), SLOT(sslErrorsOccured(QNetworkReply*,QList<QSslError>)));
  QObject::connect(&d->writeNAM, SIGNAL(finished(QNetworkReply*)), SLOT(onWriteFinished(QNetworkReply*)));
  QObject::connect(&d->writeNAM, SIGNAL(sslErrors(QNetworkReply*,QList<QSslError>)), SLOT(sslErrorsOccured(QNetworkReply*,QList<QSslError>)));

  ui->attachmentTableWidget->installEventFilter(this);
  ui->attachmentTableWidget->setColumnCount(2);
  ui->attachmentTableWidget->setHorizontalHeaderLabels(QStringList() << tr("Filename") << tr("Size"));
  ui->attachmentTableWidget->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  d->attachmentsContextMenu = new QMenu(ui->attachmentTableWidget);
  d->actionAttachFile = new QAction(QIcon(":/images/filenew.png"),
                                    tr("Attach files ..."),
                                    d->attachmentsContextMenu);
  d->attachmentsContextMenu->addAction(d->actionAttachFile);
  d->attachmentsContextMenu->addSeparator();
  d->actionSaveAttachment = new QAction(QIcon(":/images/filesave.png"),
                                        tr("Save attachment as ..."),
                                        d->attachmentsContextMenu);
  d->attachmentsContextMenu->addAction(d->actionSaveAttachment);
  d->actionDeleteAttachment = new QAction(QIcon(":/images/remove.png"),
                                          tr("Delete attachments"),
                                          d->attachmentsContextMenu);
  d->attachmentsContextMenu->addAction(d->actionDeleteAttachment);
  QObject::connect(ui->addAttachmentPushButton, SIGNAL(clicked(bool)), SLOT(onAttachFile()));

  d->trayMenu = new QMenu(AppName);
  QObject::connect(&d->trayIcon, SIGNAL(activated(QSystemTrayIcon::ActivationReason)), SLOT(trayIconActivated(QSystemTrayIcon::ActivationReason)));
  d->actionShow = d->trayMenu->addAction(tr("Minimize window"));
  QObject::connect(d->actionShow, SIGNAL(triggered(bool)), SLOT(showHide()));
  QAction *actionSync = d->trayMenu->addAction(tr("Sync"));
  QObject::connect(actionSync, SIGNAL(triggered(bool)), SLOT(onSync()));
  QAction *actionClearClipboard = d->trayMenu->addAction(tr("Clear clipboard"));
  QObject::connect(actionClearClipboard, SIGNAL(triggered(bool)), SLOT(clearClipboard()));
  d->actionLockApplication = d->trayMenu->addAction(tr("Lock application ..."));
  QObject::connect(d->actionLockApplication, SIGNAL(triggered(bool)), SLOT(lockApplication()));
  QAction *actionAbout = d->trayMenu->addAction(tr("About %1").arg(AppName));
  QObject::connect(actionAbout, SIGNAL(triggered(bool)), SLOT(about()));
  QAction *actionQuit = d->trayMenu->addAction(tr("Quit"));
  QObject::connect(actionQuit, SIGNAL(triggered(bool)), SLOT(close()));
  d->trayIcon.setContextMenu(d->trayMenu);
  d->trayIcon.show();

  d->pwdLabelOpacityEffect = new QGraphicsOpacityEffect(ui->passwordLengthLabel);
  d->pwdLabelOpacityEffect->setOpacity(0.5);
  ui->passwordLengthLabel->setGraphicsEffect(d->pwdLabelOpacityEffect);

  QLayout *moreSettingsGroupBoxLayout = ui->moreSettingsGroupBox->layout();
  d->expandableGroupBox->setLayout(moreSettingsGroupBoxLayout);
  d->expandableGroupBox->setTitle(ui->moreSettingsGroupBox->title());
  ui->generatedPasswordTab->layout()->addWidget(d->expandableGroupBox);
  ui->moreSettingsGroupBox->hide();
  QObject::connect(d->expandableGroupBox, SIGNAL(expansionStateChanged()), SLOT(onExpandableCheckBoxStateChanged()));

  ui->statusBar->addPermanentWidget(d->countdownWidget);
  setDirty(false);
  ui->tabWidget->setCurrentIndex(TabGeneratedPassword);
  enterMasterPassword();
}


void MainWindow::showHide(void)
{
  Q_D(MainWindow);
  if (d->masterPasswordDialog->isVisible()) {
    d->masterPasswordDialog->raise();
    d->masterPasswordDialog->activateWindow();
    d->masterPasswordDialog->setFocus();
  }
  else if (this->isMinimized()) {
    show();
    showNormal();
    raise();
    activateWindow();
    setFocus();
    d->actionShow->setText(tr("Minimize window"));
  }
  else {
    showMinimized();
    d->actionShow->setText(tr("Restore window"));
  }
}


void MainWindow::trayIconActivated(QSystemTrayIcon::ActivationReason reason)
{
  if (reason == QSystemTrayIcon::DoubleClick) {
    showHide();
  }
}


MainWindow::~MainWindow()
{
  delete ui;
}


QSize MainWindow::sizeHint(void) const
{
  return QSize(340, 400);
}


QSize MainWindow::minimumSizeHint(void) const
{
  return QSize(324, 391);
}


void MainWindow::prepareExit(void)
{
  Q_D(MainWindow);
  _LOG("MainWindow::prepareExit()");
  d->trayIcon.hide();
  d->optionsDialog->close();
  d->changeMasterPasswordDialog->close();
  d->masterPasswordDialog->close();
  invalidateMasterPassword(false);
  if (d->lockFile->isLocked()) {
    d->lockFile->unlock();
  }
  saveUiSettings();
}


void MainWindow::closeEvent(QCloseEvent *e)
{
  Q_D(MainWindow);
  cancelPasswordGeneration();
  d->backupFileDeletionFuture.waitForFinished();
  if (d->parameterSetDirty && !ui->domainsComboBox->currentText().isEmpty()) {
    QMessageBox::StandardButton button = saveYesNoCancel();
    switch (button) {
    case QMessageBox::Yes:
      saveCurrentDomainSettings();
      saveSettings();
      // fall-through
    case QMessageBox::No:
      prepareExit();
      e->accept();
      break;
    case QMessageBox::Cancel:
      e->ignore();
      break;
    default:
      qWarning() << "Oops! Should never have come here. button =" << button;
      break;
    }
  }
  else {
    prepareExit();
    e->accept();
  }
}


void MainWindow::changeEvent(QEvent *e)
{
  QMainWindow::changeEvent(e);
  switch (e->type()) {
  case QEvent::WindowStateChange:
  {
    if (windowState() & Qt::WindowMinimized) {
      QTimer::singleShot(200, this, SLOT(showMinimized()));
    }
    break;
  }
  default:
    break;
  }
}


void MainWindow::resizeEvent(QResizeEvent *)
{
  // ...
}


bool MainWindow::event(QEvent *e)
{
  switch (e->type()) {
  case QEvent::Move:
    // fall-through
  case QEvent::Resize:
    // fall-through
  case QEvent::MouseButtonPress:
    // fall-through
  case QEvent::KeyPress:
    restartInvalidationTimer();
    break;
  default:
    break;
  }
  return QMainWindow::event(e);
}


void MainWindow::resetAllFieldsExceptDomainComboBox(void)
{
  Q_D(MainWindow);
  // qDebug() << "MainWindow::resetAllFieldsExceptDomainComboBox()";

  ui->userLineEdit->blockSignals(true);
  ui->userLineEdit->setText(QString());
  ui->userLineEdit->blockSignals(false);

  ui->urlLineEdit->blockSignals(true);
  ui->urlLineEdit->setText(QString());
  ui->urlLineEdit->blockSignals(false);

  ui->legacyPasswordLineEdit->blockSignals(true);
  ui->legacyPasswordLineEdit->setText(QString());
  ui->legacyPasswordLineEdit->blockSignals(false);

  ui->generatedPasswordLineEdit->setText(QString());

  ui->saltBase64LineEdit->blockSignals(true);
  renewSalt();
  ui->saltBase64LineEdit->blockSignals(false);

  ui->iterationsSpinBox->blockSignals(true);
  ui->iterationsSpinBox->setValue(d->optionsDialog->defaultIterations());
  ui->iterationsSpinBox->blockSignals(false);

  ui->notesPlainTextEdit->blockSignals(true);
  ui->notesPlainTextEdit->setPlainText(QString());
  ui->notesPlainTextEdit->blockSignals(false);

  ui->deleteCheckBox->blockSignals(true);
  ui->deleteCheckBox->setChecked(false);
  ui->deleteCheckBox->blockSignals(false);

  ui->createdLabel->setText(QString());
  ui->modifiedLabel->setText(QString());

  ui->extraLineEdit->blockSignals(true);
  ui->extraLineEdit->setText(Password::ExtraChars);
  ui->extraLineEdit->blockSignals(false);

  ui->easySelectorWidget->blockSignals(true);
  ui->easySelectorWidget->setLength(d->optionsDialog->defaultPasswordLength());
  ui->easySelectorWidget->setComplexityValue(Password::DefaultComplexityValue);
  ui->easySelectorWidget->setExtraCharacters(ui->extraLineEdit->text());
  ui->easySelectorWidget->blockSignals(false);

  ui->attachmentTableWidget->setRowCount(0);

  applyComplexity(ui->easySelectorWidget->complexityValue());
}


void MainWindow::resetAllFields(void)
{
  Q_D(MainWindow);
  resetAllFieldsExceptDomainComboBox();
  ui->domainsComboBox->setEditable(true);
  ui->domainsComboBox->setCompleter(d->completer);
  ui->domainsComboBox->setCurrentIndex(-1);
  ui->domainsComboBox->setFocus();
  setDirty(false);
}


int MainWindow::findDomainInComboBox(const QString &domain, int lo, int hi) const
{
  if (hi < lo) {
    return NotFound;
  }
  const int idx = (lo + hi) / 2;
  const int c = ui->domainsComboBox->itemText(idx).compare(domain, Qt::CaseInsensitive);
  if (c > 0) {
    return findDomainInComboBox(domain, lo, idx - 1);
  }
  else if (c < 0) {
    return findDomainInComboBox(domain, idx + 1, hi);
  }
  return idx;
}


int MainWindow::findDomainInComboBox(const QString &domain) const
{
  return findDomainInComboBox(domain, 0, ui->domainsComboBox->count());
}


bool MainWindow::domainComboboxContains(const QString &domain) const
{
  return findDomainInComboBox(domain, 0, ui->domainsComboBox->count()) != NotFound;
}


void MainWindow::renewSalt(void)
{
  Q_D(MainWindow);
  const QByteArray &salt = Crypter::randomBytes(d->optionsDialog->saltLength());
  ui->saltBase64LineEdit->setText(salt.toBase64());
}


void MainWindow::onRenewSalt(void)
{
  if (ui->domainsComboBox->currentText().isEmpty())
    return;
  int button = QMessageBox::Yes;
  if (domainComboboxContains(ui->domainsComboBox->currentText())) {
    button = QMessageBox::question(
          this,
          tr("Really regenerate?"),
          tr("Your password is about to be regenerated. "
             "This is useful if you suspect that your credential have been compromised. "
             "You cannot undo this action. "
             "Are you sure you want to generate a new password?"),
          QMessageBox::Yes,
          QMessageBox::No);
  }
  if (button == QMessageBox::Yes) {
    renewSalt();
  }
}


QMessageBox::StandardButton MainWindow::saveYesNoCancel(void)
{
  Q_D(MainWindow);
  d->interactionSemaphore.acquire();
  QMessageBox::StandardButton button = QMessageBox::question(
        this,
        tr("Save changes?"),
        tr("You have changed the current domain settings. "
           "Do you want to save the changes before proceeding?"),
        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
        QMessageBox::Yes);
  d->interactionSemaphore.release();
  return button;
}


void MainWindow::cancelPasswordGeneration(void)
{
  Q_D(MainWindow);
#if HACKING_MODE_ENABLED
  if (d->hackingMode) {
    d->hackingMode = false;
    ui->renewSaltPushButton->setEnabled(true);
    ui->legacyPasswordLineEdit->setReadOnly(false);
  }
#endif
  stopPasswordGeneration();
}


void MainWindow::setDirty(bool dirty)
{
  Q_D(MainWindow);
  d->parameterSetDirty = dirty;
  ui->shuffleUsernamePushButton->setVisible(!dirty && ui->domainsComboBox->currentText().isEmpty());
  if (d->parameterSetDirty) {
    d->countdownWidget->stop();
  }
  else {
    restartInvalidationTimer();
  }
  if (!ui->domainsComboBox->currentText().isEmpty() && domainComboboxContains(ui->domainsComboBox->currentText())) {
    ui->domainsComboBox->setEditable(!d->parameterSetDirty);
    ui->domainsComboBox->setCompleter(d->parameterSetDirty ? Q_NULLPTR : d->completer);
  }
  ui->savePushButton->setEnabled(d->parameterSetDirty);
  ui->revertPushButton->setEnabled(d->parameterSetDirty);
  ui->actionLockApplication->setEnabled(!d->parameterSetDirty);
  if (d->actionLockApplication != Q_NULLPTR) {
    d->actionLockApplication->setEnabled(!d->parameterSetDirty);
  }
  if (ui->actionChangeMasterPassword != Q_NULLPTR) {
    ui->actionChangeMasterPassword->setEnabled(!d->parameterSetDirty);
  }
  updateWindowTitle();
}


void MainWindow::openURL(void)
{
  Q_D(MainWindow);
  if (!ui->urlLineEdit->text().isEmpty()) {
    QDesktopServices::openUrl(QUrl(ui->urlLineEdit->text()));
    copyUsernameToClipboard();
  }
}


void MainWindow::onURLChanged(QString)
{
  setDirty(true);
  bool urlFieldFilled = !ui->urlLineEdit->text().isEmpty();
  ui->openURLPushButton->setEnabled(urlFieldFilled);
  ui->loginPushButton->setEnabled(urlFieldFilled);
}


void MainWindow::onUserChanged(QString)
{
  setDirty(true);
  updatePassword();
}


void MainWindow::onUsedCharactersChanged(void)
{
  setDirty(true);
  updatePassword();
}


void MainWindow::onExtraCharactersChanged(QString)
{
  Q_D(MainWindow);
  setDirty(true);
  setTemplate();
  updatePassword();
}


void MainWindow::onPasswordLengthChanged(int len)
{
  Q_D(MainWindow);
  setDirty(true);
  ui->easySelectorWidget->setLength(len);
  updatePassword();
}


void MainWindow::onIterationsChanged(int)
{
  setDirty(true);
  updatePassword();
}


void MainWindow::onSaltChanged(QString)
{
  setDirty(true);
  restartInvalidationTimer();
  updatePassword();
}


void MainWindow::onDeleteChanged(bool)
{
  setDirty(true);
  restartInvalidationTimer();
}


void MainWindow::restartInvalidationTimer(void)
{
  Q_D(MainWindow);
  const int timeout = d->optionsDialog->masterPasswordInvalidationTimeMins();
  if (timeout > 0 && !d->parameterSetDirty) {
    d->countdownWidget->start(1000 * timeout * 60);
  }
  else {
    d->countdownWidget->stop();
  }
}


void MainWindow::onPasswordGenerationStarted(void)
{
  // do nothing
}


void MainWindow::updatePassword(void)
{
  Q_D(MainWindow);
  // qDebug() << "MainWindow::updatePassword() triggered by" << (sender() ? sender()->objectName() : "NONE");
  if (!d->masterPassword.isEmpty()) {
    if (ui->legacyPasswordLineEdit->text().isEmpty()) {
      stopPasswordGeneration();
#if HACKING_MODE_ENABLED
      if (!d->hackingMode) {
        ui->generatedPasswordLineEdit->setText(QString());
        ui->statusBar->showMessage(QString());
      }
#endif
      d->password.generateAsync(d->KGK, collectedDomainSettings());
    }
    else {
      ui->generatedPasswordLineEdit->setText(QString());
    }
    restartInvalidationTimer();
  }
}


DomainSettings MainWindow::collectedDomainSettings(void) const
{
  DomainSettings ds;
  ds.domainName = ui->domainsComboBox->currentText();
  ds.url = ui->urlLineEdit->text();
  ds.deleted = ui->deleteCheckBox->isChecked();
  ds.createdDate = d_ptr->createdDate.isValid() ? d_ptr->createdDate : QDateTime::currentDateTime();
  ds.modifiedDate = d_ptr->modifiedDate;
  ds.userName = ui->userLineEdit->text();
  ds.notes = ui->notesPlainTextEdit->toPlainText();
  ds.salt_base64 = ui->saltBase64LineEdit->text();
  ds.legacyPassword = ui->legacyPasswordLineEdit->text();
  ds.iterations = ui->iterationsSpinBox->value();
  ds.extraCharacters = ui->extraLineEdit->text();
  ds.passwordTemplate = ui->passwordTemplateLineEdit->text();
  QVariantMap attachedFiles;
  for (int row = 0; row < ui->attachmentTableWidget->rowCount(); ++row) {
    QTableWidgetItem *const item = ui->attachmentTableWidget->item(row, 0);
    if (item != Q_NULLPTR) {
      attachedFiles[item->text()] = item->data(Qt::UserRole);
    }
  }
  ds.files = attachedFiles;
  ds.tags = QStringList(); // TODO: implement tagging facility
#ifndef OMIT_V2_CODE
  if (DomainSettings::isV2Template(ds.passwordTemplate)) {
    ds.usedCharacters = ui->extraLineEdit->text();
  }
#endif
  return ds;
}


void MainWindow::updateCheckableLabel(QLabel *label, bool checked)
{
  static const QPixmap CheckedPixmap(":/images/check.png");
  static const QPixmap UncheckedPixmap(":/images/uncheck.png");
  label->setPixmap(checked ? CheckedPixmap : UncheckedPixmap);
  label->setEnabled(checked);
}


void MainWindow::applyComplexity(int complexityValue)
{
  const Password::Complexity &complexity = Password::Complexity::fromValue(complexityValue);
  updateCheckableLabel(ui->useDigitsLabel, complexity.digits);
  updateCheckableLabel(ui->useLowercaseLabel, complexity.lowercase);
  updateCheckableLabel(ui->useUppercaseLabel, complexity.uppercase);
  updateCheckableLabel(ui->useExtraLabel, complexity.extra);
}


void MainWindow::onLogin(void)
{
  Q_D(MainWindow);
  const SecureString &pwd = ui->generatedPasswordLineEdit->text().isEmpty()
      ? ui->legacyPasswordLineEdit->text()
      : ui->generatedPasswordLineEdit->text();
  d->tcpClient.connect(ui->urlLineEdit->text(), ui->userLineEdit->text(), pwd);
  restartInvalidationTimer();
}


void MainWindow::onMessageFromTcpClient(QJsonDocument json)
{
  QVariantMap msg = json.toVariant().toMap();
  if (msg["status"].toString() != "ok") {
    ui->statusBar->showMessage(tr("Error: %1").arg(msg["message"].toString()), 2000);
  }
  else {
    ui->statusBar->showMessage(msg["message"].toString(), 2000);
  }
}


void MainWindow::applyTemplateStringToGUI(const QString &t)
{
  Q_D(MainWindow);
  const QStringList &templateParts = t.split(';', QString::KeepEmptyParts);
  QString templ;
  if (templateParts.count() == 1) {
    templ = templateParts.at(0);
  }
  else if (templateParts.count() == 2) {
    templ = templateParts.at(1);
  }
  if (!templ.isEmpty()) {
    const int length = templ.length();
    const int complexityValue = Password::Complexity::fromTemplate(templ).value();
    ui->easySelectorWidget->blockSignals(true);
    ui->easySelectorWidget->setLength(length);
    ui->easySelectorWidget->setComplexityValue(complexityValue);
    ui->easySelectorWidget->blockSignals(false);
    applyComplexity(complexityValue);
  }
}


void MainWindow::setTemplate(void)
{
  Q_D(MainWindow);
  QString usedCharacters;
  if (ui->useDigitsLabel->isEnabled()) {
    usedCharacters += 'n';
  }
  if (ui->useLowercaseLabel->isEnabled()) {
    usedCharacters += 'a';
  }
  if (ui->useUppercaseLabel->isEnabled()) {
    usedCharacters += 'A';
  }
  if (ui->useExtraLabel->isEnabled()) {
    usedCharacters += 'o';
  }
  QString pwdTemplate = usedCharacters + QByteArray(ui->easySelectorWidget->length() - usedCharacters.count(), 'x');
  ui->passwordTemplateLineEdit->setText(shuffled(pwdTemplate));
  ui->easySelectorWidget->setExtraCharacters(ui->extraLineEdit->text());
}


void MainWindow::stopPasswordGeneration(void)
{
  Q_D(MainWindow);
  if (d->password.isRunning()) {
    d->password.abortGeneration();
    d->password.waitForFinished();
  }
}


void MainWindow::changeMasterPassword(void)
{
  Q_D(MainWindow);
  d->changeMasterPasswordDialog->setPasswordFilename(d->optionsDialog->passwordFilename());
  d->interactionSemaphore.acquire();
  const int button = d->changeMasterPasswordDialog->exec();
  d->interactionSemaphore.release();
  if ((button == QDialog::Accepted) && (d->changeMasterPasswordDialog->oldPassword() == d->masterPassword)) {
    if (d->optionsDialog->syncToServerEnabled() || d->optionsDialog->syncToFileEnabled()) {
      d->masterPasswordChangeStep = 1;
      nextChangeMasterPasswordStep();
    }
    else {
      saveAllDomainDataToSettings();
      d->masterPassword = d->changeMasterPasswordDialog->newPassword();
      d->keyGenerationFuture.waitForFinished();
      generateSaltKeyIV().waitForFinished();
      cleanupAfterMasterPasswordChanged();
    }
  }
}


void MainWindow::nextChangeMasterPasswordStep(void)
{
  Q_D(MainWindow);
  switch (d->masterPasswordChangeStep++) {
  case 1:
    d->progressDialog->show();
    d->progressDialog->raise();
    d->progressDialog->setText(tr("Starting synchronisation ..."));
    d->progressDialog->setRange(1, 3);
    d->progressDialog->setValue(1);
    saveAllDomainDataToSettings();
    onSync();
    if (!d->optionsDialog->syncToServerEnabled()) {
      nextChangeMasterPasswordStep();
    }
    break;
  case 2:
    d->progressDialog->setValue(2);
    d->masterPassword = d->changeMasterPasswordDialog->newPassword();
    generateSaltKeyIV().waitForFinished();
    d->progressDialog->setText(tr("Writing to sync peers ..."));
    if (d->optionsDialog->useSyncFile()) {
      writeToRemote(SyncPeerFile);
      if (!d->optionsDialog->syncToServerEnabled()) {
        nextChangeMasterPasswordStep();
      }
    }
    if (d->optionsDialog->syncToServerEnabled()) {
      writeToRemote(SyncPeerServer);
    }
    break;
  case 3:
    d->masterPasswordChangeStep = 0;
    d->progressDialog->setText(tr("Password changed."));
    d->progressDialog->setValue(3);
    cleanupAfterMasterPasswordChanged();
    break;
  default:
    // ignore
    break;
  }
}


#if HACKING_MODE_ENABLED
static QString makeHMS(qint64 ms)
{
  QString sign;
  if (ms < 0) {
    sign = "-";
    ms = -ms;
  }
  else {
    sign = "";
  }
  qint64 secs = ms / 1000;
  qint64 hrs = secs / 60 / 60;
  qint64 mins = (secs / 60 - hrs * 60);
  secs -= 60 * (hrs * 60 + mins);
  return QString("%1%2h%3'%4\"")
      .arg(sign)
      .arg(hrs)
      .arg(mins, 2, 10, QChar('0'))
      .arg(secs, 2, 10, QChar('0'));
}
#endif

void MainWindow::onPasswordGenerated(void)
{
  Q_D(MainWindow);
#if HACKING_MODE_ENABLED
  if (!d->hackingMode) {
#endif
    ui->generatedPasswordLineEdit->setText(d->password.password());
    ui->passwordLengthLabel->setText(tr("(%1 characters)").arg(d->password.password().length()));
    d->pwdLabelOpacityEffect->setOpacity(1);
    if (!d->password.isAborted()) {
      ui->statusBar->showMessage(tr("generation time: %1 ms")
                                 .arg(1e3 * d->password.elapsedSeconds(), 0, 'f', 4), 3000);
    }
#if HACKING_MODE_ENABLED
  }
  else { // in hacking mode
    ui->generatedPasswordLineEdit->setText(d->password());
    PositionTable st(d->password());
    if (d->hackPos == st) {
      const QString &newCharTable = d->hackPos.substitute(st, usedCharacters());
      ui->usedCharactersPlainTextEdit->setPlainText(newCharTable);
      d->hackingMode = false;
      ui->renewSaltPushButton->setEnabled(true);
      ui->legacyPasswordLineEdit->setReadOnly(false);
      hideActivityIcons();
      int button = QMessageBox::question(
            this,
            tr("Finished \"hacking\""),
            tr("Found a salt in %1 that allows to calculate the legacy password from the domain settings :-) "
               "The legacy password is no longer needed. "
               "Do you want to clear the legacy password and save the new domain settings?").arg(makeHMS(d->hackClock.elapsed())));
      if (button == QMessageBox::Yes) {
        ui->legacyPasswordLineEdit->setText(QString());
        ui->tabWidget->setCurrentIndex(0);
        saveCurrentDomainSettings();
      }
      restartInvalidationTimer();
    }
    else {
      const qint64 dt = d->hackIterationClock.restart();
      d->hackIterationDurationMs = (d->hackIterationDurationMs > 0)
          ? (d->hackIterationDurationMs + dt) / 2
          : dt;
      ui->statusBar->showMessage(
            tr("Hacking ... t%1 (%2ms) t: %3")
            .arg(makeHMS(d->hackClock.elapsed() - 3 * d->hackPermutations * d->hackIterationDurationMs / 2))
            .arg(dt)
            .arg(makeHMS(d->hackClock.elapsed()))
            );
      incrementEndianless(d->hackSalt);
      ui->saltBase64LineEdit->setText(d->hackSalt.toBase64());
    }
  }
#endif
}


void MainWindow::onPasswordGenerationAborted(void)
{
  onPasswordGenerated();
}


void MainWindow::onServerCertificatesUpdated(const QList<QSslCertificate> &certs)
{
  Q_D(MainWindow);
  d->deleteNAM.clearAccessCache();
  d->readNAM.clearAccessCache();
  d->writeNAM.clearAccessCache();
  d->resetSSLConf();
  if (!certs.isEmpty()) {
    d->sslConf.setCaCertificates(certs);
  }
}


void MainWindow::showOptionsDialog(void)
{
  Q_D(MainWindow);
  d->interactionSemaphore.acquire();
  const int button = d->optionsDialog->exec();
  d->interactionSemaphore.release();
  if (button == QDialog::Accepted) {
    saveSyncDataToSettings();
    saveUiSettings();
  }
}


QFuture<void> &MainWindow::generateSaltKeyIV(void)
{
  Q_D(MainWindow);
  // qDebug() << "MainWindow::generateSaltKeyIV()";
  _LOG("MainWindow::generateSaltKeyIV() ...");
  d->keyGenerationFuture = QtConcurrent::run(this, &MainWindow::generateSaltKeyIVThread);
  return d->keyGenerationFuture;
}


void MainWindow::generateSaltKeyIVThread(void)
{
  Q_D(MainWindow);
  Q_ASSERT_X(!d->masterPassword.isEmpty(), "MainWindow::generateSaltKeyIVThread()", "d->masterPassword must not be empty");
  if (d->masterPassword.isEmpty()) {
    qWarning() << "Error in  MainWindow::generateSaltKeyIVThread(): d->masterPassword must not be empty";
    return;
  }
  QMutexLocker(&d->keyGenerationMutex);
  d->salt = Crypter::generateSalt();
  Crypter::makeKeyAndIVFromPassword(d->masterPassword.toUtf8(), d->salt, d->masterKey, d->IV);
  emit saltKeyIVGenerated();
}


void MainWindow::onGeneratedSaltKeyIV(void)
{
  Q_D(MainWindow);
  _LOG("MainWindow::onGeneratedSaltKeyIV()");
  ui->statusBar->showMessage(tr("Auto-generated new salt (%1) and key.").arg(QString::fromLatin1(d->salt.mid(0, 4).toHex())), 2000);
}


static const QString KGKFileExtension = QObject::tr("KGK file (*.pem *.kgk)");


void MainWindow::onExportKGK(void)
{
  Q_D(MainWindow);
  int rc = QMessageBox::question(this,
                                 tr("Security hint"),
                                 tr("You're about to export your key generation key (KGK). "
                                    "The KGK is used to derive passwords from your master password "
                                    "and to derive a key to encrypt your settings. "
                                    "You normally won't export the KGK unless for backup purposes. "
                                    "The KGK is encrypted with a key derived from your master password. "
                                    "Are you prepared for this?"));
  if (rc == QMessageBox::Yes) {
    QString kgkFilename = QFileDialog::getSaveFileName(this, tr("Export KGK to ..."), QString(), KGKFileExtension);
    if (!kgkFilename.isEmpty()) {
      Exporter(kgkFilename).write(d->KGK, d->masterPassword.toUtf8());
    }
  }
}


void MainWindow::onImportKGK(void)
{
  Q_D(MainWindow);
  int rc = QMessageBox::question(this,
                                 tr("Read carefully before proceeding!"),
                                 tr("You are about to import a previously saved key generation key (KGK). "
                                    "This should only be done if absolutely necessary, e.g. "
                                    "to restore a damaged settings file. This is because changing the KGK "
                                    "will also change the generated passwords. "
                                    "Are you really sure you want to import a KGK?"));
  if (rc == QMessageBox::Yes) {
    QString kgkFilename = QFileDialog::getOpenFileName(this, tr("Import KGK from ..."), QString(), KGKFileExtension);
    if (!kgkFilename.isEmpty()) {
      SecureByteArray kgk = Exporter(kgkFilename).read(d->masterPassword.toUtf8());
      if (kgk.size() == Crypter::KGKSize) {
        d->KGK = kgk;
        QMessageBox::information(this,
                                 tr("KGK imported"),
                                 tr("KGK successfully imported. Your generated passwords may have changed. "
                                    "Please check if they are still valid, or valid again."));
        resetAllFields();
      }
      else {
        QMessageBox::warning(this,
                             tr("Bad KGK"),
                             tr("The KGK you've loaded is malformed. "
                                "It shall be %1 byte long, but is in fact %2 byte long. "
                                "The KGK will not be imported and "
                                "your settings will not be changed.")
                             .arg(Crypter::KGKSize).arg(kgk.size()));
      }
    }
  }
}


QString MainWindow::selectAlternativeDomainNameFor(const QString &domainName)
{
  Q_D(MainWindow);
  QString newDomainName = domainName;
  int idx = 0;
  while (findDomainInComboBox(newDomainName) != NotFound) {
    newDomainName = QString("%1 (%2)").arg(domainName).arg(++idx);
  }
  return newDomainName;
}


QString MainWindow::selectAlternativeDomainNameFor(const QString &domainName, const QStringList &domainNameList)
{
  QString newDomainName;
  int idx = 0;
  do {
    newDomainName = QString("%1 (%2)").arg(domainName).arg(++idx);
  }
  while (domainNameList.contains(newDomainName));
  return newDomainName;
}


void MainWindow::onImportKeePass2XmlFile(void)
{
  Q_D(MainWindow);
  const QString &kp2xmlFilename = QFileDialog::getOpenFileName(this, tr("Import KeePass 2 XML file"), QString(), "KeePass 2 XML (*.xml)");
  if (kp2xmlFilename.isEmpty())
    return;
  QFileInfo fi(kp2xmlFilename);
  if (fi.isReadable() && fi.isFile()) {
    KeePass2XmlReader reader(kp2xmlFilename);
    if (!reader.isValid()) {
      if (!reader.xmlErrorString().isEmpty()) {
      QMessageBox::warning(this,
                           tr("Invalid KeePass 2 XML file"),
                           tr("The selected KeePass 2 XML file doesn't contain valid XML: %1 (line %2, column: %3)")
                           .arg(reader.xmlErrorString()).arg(reader.xmlErrorLine()).arg(reader.xmlErrorColumn()));
      }
      else {
        QMessageBox::warning(this,
                             tr("Cannot read KeePass 2 XML file"),
                             tr("The selected KeePass 2 XML file cannot be read: %1")
                             .arg(reader.errorString()));
      }
      return;
    }
    typedef QPair<QString, QString> StringPair;
    QList<StringPair> renamed;
    foreach (DomainSettings ds, reader.domains()) {
      QString newDomainName = selectAlternativeDomainNameFor(ds.domainName);
      if (newDomainName != ds.domainName)
        renamed.append(qMakePair(ds.domainName, newDomainName));
      ds.domainName = newDomainName;
      d->domains.append(ds);
    }
    DomainSettings currentDomainSettings = d->domains.at(ui->domainsComboBox->currentText());
    makeDomainComboBox();
    if (!currentDomainSettings.isEmpty()) {
      copyDomainSettingsToGUI(currentDomainSettings);
    }
    QString msgBoxText;
    if (reader.domains().count() == 1) {
      msgBoxText = tr("<p>%1 domain has been imported successfully from the KeePass 2 XML file.</p>")
        .arg(reader.domains().count());
    }
    else {
      msgBoxText = tr("<p>%1 domains have been imported successfully from the KeePass 2 XML file.</p>")
        .arg(reader.domains().count());
    }
    if (renamed.count() > 0) {
      if (renamed.count() == 1) {
        msgBoxText += tr("<p>%1 domain had to be renamed:</p>").arg(renamed.count());
      }
      else {
        msgBoxText += tr("<p>%1 domains had to be renamed:</p>").arg(renamed.count());
      }
      msgBoxText += "<ul>";
      foreach (StringPair r, renamed) {
        msgBoxText += "<li>" + r.first + " >> " + r.second + "</li>";
      }
      msgBoxText += "</ul>";
    }
    QMessageBox::information(this, tr("Import successful"), msgBoxText);
  }
}


void MainWindow::onImportPasswordSafeFile(void)
{
  Q_D(MainWindow);
  const QString &psFilename = QFileDialog::getOpenFileName(this, tr("Import PasswordSafe file"), QString(), "PasswordSafe (*.txt)");
  if (psFilename.isEmpty())
    return;
  QFileInfo fi(psFilename);
  if (fi.isReadable() && fi.isFile()) {
    PasswordSafeReader reader(psFilename);
    if (!reader.isValid()) {
      if (!reader.dataErrorString().isEmpty()) {
      QMessageBox::warning(this,
                           tr("Invalid PasswordSafe file"),
                           tr("The selected PasswordSafe file doesn't contain valid data: %1 (line %2, column: %3)")
                           .arg(reader.dataErrorString()).arg(reader.errorLine()).arg(reader.errorColumn()));
      }
      else {
        QMessageBox::warning(this,
                             tr("Cannot read PasswordSafe file"),
                             tr("The selected PasswordSafe file cannot be read: %1")
                             .arg(reader.errorString()));
      }
      return;
    }
    typedef QPair<QString, QString> StringPair;
    QList<StringPair> renamed;
    foreach (DomainSettings ds, reader.domains()) {
      QString newDomainName = selectAlternativeDomainNameFor(ds.domainName);
      if (newDomainName != ds.domainName)
        renamed.append(qMakePair(ds.domainName, newDomainName));
      ds.domainName = newDomainName;
      d->domains.append(ds);
    }
    DomainSettings currentDomainSettings = d->domains.at(ui->domainsComboBox->currentText());
    saveAllDomainDataToSettings();
    makeDomainComboBox();
    if (!currentDomainSettings.isEmpty()) {
      copyDomainSettingsToGUI(currentDomainSettings);
    }
    QString msgBoxText;
    if (reader.domains().count() == 1) {
      msgBoxText = tr("<p>%1 domain has been imported successfully from the PasswordSafe file.</p>")
        .arg(reader.domains().count());
    }
    else {
      msgBoxText = tr("<p>%1 domains have been imported successfully from the PasswordSafe file.</p>")
        .arg(reader.domains().count());
    }
    if (renamed.count() > 0) {
      if (renamed.count() == 1) {
        msgBoxText += tr("<p>%1 domain had to be renamed:</p>").arg(renamed.count());
      }
      else {
        msgBoxText += tr("<p>%1 domains had to be renamed:</p>").arg(renamed.count());
      }
      msgBoxText += "<ul>";
      foreach (StringPair r, renamed) {
        msgBoxText += "<li>" + r.first + " >> " + r.second + "</li>";
      }
      msgBoxText += "</ul>";
    }
    QMessageBox::information(this, tr("Import successful"), msgBoxText);
  }
}


void MainWindow::copyUsernameToClipboard(void)
{
  QApplication::clipboard()->setText(ui->userLineEdit->text());
  ui->statusBar->showMessage(tr("Username copied to clipboard."), 5000);
}


void MainWindow::copyGeneratedPasswordToClipboard(void)
{
  QApplication::clipboard()->setText(ui->generatedPasswordLineEdit->text());
  ui->statusBar->showMessage(tr("Generated password copied to clipboard."), 3000);
}


void MainWindow::copyLegacyPasswordToClipboard(void)
{
  QApplication::clipboard()->setText(ui->legacyPasswordLineEdit->text());
  ui->statusBar->showMessage(tr("Legacy password copied to clipboard."), 5000);
}


void MainWindow::copyDomainSettingsToGUI(DomainSettings ds)
{
  Q_D(MainWindow);
  // qDebug() << "MainWindow::copyDomainSettingsToGUI(...) for domain" << ds.domainName;
  ui->domainsComboBox->blockSignals(true);
  ui->domainsComboBox->setCurrentText(ds.domainName);
  ui->domainsComboBox->blockSignals(false);
  ui->urlLineEdit->setText(ds.url);
  ui->userLineEdit->blockSignals(true);
  ui->userLineEdit->setText(ds.userName);
  ui->userLineEdit->blockSignals(false);
  ui->legacyPasswordLineEdit->setText(ds.legacyPassword);
  ui->saltBase64LineEdit->blockSignals(true);
  ui->saltBase64LineEdit->setText(ds.salt_base64);
  ui->saltBase64LineEdit->blockSignals(false);
  ui->notesPlainTextEdit->blockSignals(true);
  ui->notesPlainTextEdit->setPlainText(ds.notes);
  ui->notesPlainTextEdit->blockSignals(false);
  ui->iterationsSpinBox->blockSignals(true);
  ui->iterationsSpinBox->setValue(ds.iterations);
  ui->iterationsSpinBox->blockSignals(false);
  setAttachments(ds.files);
  ui->createdLabel->setText(ds.createdDate.toString(Qt::ISODate));
  ui->modifiedLabel->setText(ds.modifiedDate.toString(Qt::ISODate));
  d->createdDate = ds.createdDate;
  d->modifiedDate = ds.modifiedDate;
  ui->deleteCheckBox->setChecked(false);
#ifndef OMIT_V2_CODE
  if (!ds.deleted) {
    QString templ;
    const QStringList &templateParts = ds.passwordTemplate.split(';', QString::KeepEmptyParts);
    if (templateParts.size() == 1) {
      templ = templateParts.at(0);
    }
    else if (templateParts.size() == 2) {
      // v2 complexity value at index 0 ignored
      templ = templateParts.at(1);
    }
    if (ds.legacyPassword.isEmpty() && DomainSettings::isV2Template(ds.passwordTemplate)) {
      ds.extraCharacters = ds.usedCharacters;
      ds.usedCharacters.clear();
      templ[0] = 'o';
    }
    ds.passwordTemplate = templ;
  }
#endif
  ui->extraLineEdit->blockSignals(true);
  ui->extraLineEdit->setText(ds.extraCharacters);
  ui->extraLineEdit->blockSignals(false);
  ui->passwordTemplateLineEdit->blockSignals(true);
  ui->passwordTemplateLineEdit->setText(ds.passwordTemplate);
  ui->passwordTemplateLineEdit->blockSignals(false);

  if (ds.legacyPassword.isEmpty()) {
    ui->tabWidget->setCurrentIndex(TabGeneratedPassword);
    applyTemplateStringToGUI(ds.passwordTemplate);
#if HACKING_MODE_ENABLED
    ui->actionHackLegacyPassword->setEnabled(false);
#endif
  }
  else {
    ui->tabWidget->setCurrentIndex(TabLegacyPassword);
#if HACKING_MODE_ENABLED
    ui->actionHackLegacyPassword->setEnabled(true);
#endif
  }

  updatePassword();
}


void MainWindow::copyDomainSettingsToGUI(const QString &domain)
{
  Q_D(MainWindow);
  // qDebug() << "MainWindow::copyDomainSettingsToGUI(" << domain << ")";
  copyDomainSettingsToGUI(d->domains.at(domain));
}


void MainWindow::makeDomainComboBox(void)
{
  Q_D(MainWindow);
  // qDebug() << "MainWindow::makeDomainComboBox()";
  ui->domainsComboBox->blockSignals(true);
  ui->domainsComboBox->clear();
  QStringList domainNames;
  foreach(DomainSettings ds, d->domains) {
    if (!ds.deleted) {
      domainNames.append(ds.domainName);
    }
  }
  domainNames.sort(Qt::CaseInsensitive);
  ui->domainsComboBox->addItems(domainNames);
  if (d->completer != Q_NULLPTR) {
    QObject::disconnect(d->completer, SIGNAL(activated(QString)), this, SLOT(onDomainSelected(QString)));
    delete d->completer;
  }
  d->completer = new QCompleter(domainNames);
  d->completer->setCaseSensitivity(Qt::CaseInsensitive);
  QObject::connect(d->completer, SIGNAL(activated(QString)), this, SLOT(onDomainSelected(QString)));
  ui->domainsComboBox->setCompleter(d->completer);
  ui->domainsComboBox->setCurrentIndex(-1);
  ui->domainsComboBox->blockSignals(false);
}


void MainWindow::saveDomainSettings(DomainSettings ds)
{
  Q_D(MainWindow);
  // qDebug() << "MainWindow::saveDomainSettings(...) for domain" << ds.domainName;
  ui->createdLabel->setText(ds.createdDate.toString(Qt::ISODate));
  ui->modifiedLabel->setText(ds.modifiedDate.toString(Qt::ISODate));
  const QString currentDomain = ui->domainsComboBox->currentText();
  QStringList domainList;
  for (int i = 0; i < ui->domainsComboBox->count(); ++i) {
    domainList.append(ui->domainsComboBox->itemText(i));
  }
  if (domainList.contains(ds.domainName, Qt::CaseInsensitive)) {
    ds.modifiedDate = QDateTime::currentDateTime();
    if (ds.deleted) {
      domainList.removeOne(ds.domainName);
      resetAllFields();
    }
  }
  else {
    ds.createdDate = QDateTime::currentDateTime();
    ds.modifiedDate = QDateTime();
    if (!ds.deleted) {
      domainList.append(ds.domainName);
    }
  }
  d->domains.updateWith(ds);
  makeDomainComboBox();
  ui->domainsComboBox->blockSignals(true);
  ui->domainsComboBox->setCurrentText(currentDomain);
  ui->domainsComboBox->blockSignals(false);
  saveAllDomainDataToSettings();
  setDirty(false);
}


void MainWindow::saveCurrentDomainSettings(void)
{
  Q_D(MainWindow);
  // qDebug() << "MainWindow::saveCurrentDomainSettings() called by" << (sender() ? sender()->objectName() : "NONE") << "ui->domainsComboBox->currentText() =" << ui->domainsComboBox->currentText();
  if (!ui->domainsComboBox->currentText().isEmpty()) {
    restartInvalidationTimer();
    DomainSettings ds = collectedDomainSettings();
    ui->generatedPasswordLineEdit->setEchoMode(QLineEdit::Password);
    saveDomainSettings(ds);
    if (ds.deleted) {
      resetAllFields();
    }
    ui->statusBar->showMessage(tr("Domain settings saved."), 3000);
    d->lastCleanDomainSettings = ds;
  }
}


void MainWindow::onNotesChanged(void)
{
  setDirty(true);
}


void MainWindow::onLegacyPasswordChanged(QString legacyPassword)
{
  setDirty(true);
  ui->actionHackLegacyPassword->setEnabled(!legacyPassword.isEmpty());
  if (!legacyPassword.isEmpty()) {
    ui->generatedPasswordLineEdit->setText(QString());
  }
}


bool MainWindow::wipeFile(const QString &filename)
{
  Q_D(MainWindow);
  QFile f(filename);
  bool ok = f.open(QIODevice::ReadWrite | QIODevice::Unbuffered);
  const int N = f.size();
  if (ok) {
    if (d->optionsDialog->extensiveWipeout()) {
      static const int NumSinglePatterns = 16;
      static const unsigned char SinglePatterns[NumSinglePatterns] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
      };
      for (int i = 0; i < NumSinglePatterns; ++i) {
        char b = SinglePatterns[i];
        f.seek(0);
        for (int j = 0; j < N; ++j) {
          f.write(&b, 1);
        }
        f.flush();
      }
      static const int NumTriplets = 6;
      static const unsigned char Triplets[NumTriplets][3] = {
        { 0x92, 0x49, 0x24 }, { 0x49, 0x24, 0x92 }, { 0x24, 0x92, 0x49 },
        { 0x6d, 0xb6, 0xdb }, { 0xb6, 0xdb, 0x6d }, { 0xdb, 0x6d, 0xb6 }
      };
      for (int i = 0; i < NumTriplets; ++i) {
        const char *b = reinterpret_cast<const char*>(&Triplets[i][0]);
        f.seek(0);
        for (int j = 0; j < N / 3; ++j) {
          f.write(b, 3);
        }
        f.flush();
      }
    }
    f.seek(0);
    const qint64 bytesWritten = f.write(Crypter::randomBytes(N));
    ok = (bytesWritten == N);
    f.close();
    if (ok) {
      ok = f.remove();
    }
  }
  return ok;
}


void MainWindow::cleanupAfterMasterPasswordChanged(void)
{
  static const QStringList BackupFilenameFilters = { QString("*-%1-backup.txt").arg(AppName) };
  const QString &backupFilePath = QStandardPaths::writableLocation(QStandardPaths::DataLocation);
  const QStringList backupFileNames = QDir(backupFilePath).entryList(BackupFilenameFilters, QDir::Files | QDir::CaseSensitive, QDir::NoSort);
  if (!backupFileNames.isEmpty()) {
    int rc = QMessageBox::question(this,
                                   tr("Delete backup files?"),
                                   tr("You've changed your master password. "
                                      "Assuming that is has been compromised prior to that, "
                                      "all of your backup files should be deleted. "
                                      "I found %1 backup file(s) in %2. "
                                      "Do you want me to securely delete them "
                                      "and write a new backup file with the current settings?")
                                   .arg(backupFileNames.size())
                                   .arg(backupFilePath));
    if (rc == QMessageBox::Yes) {
      removeOutdatedBackupFiles();
    }
  }
}


void MainWindow::removeOutdatedBackupFilesThread(void)
{
  Q_D(MainWindow);
  const QString &backupFilePath = QStandardPaths::writableLocation(QStandardPaths::DataLocation);
  const QStringList backupFileNames = QDir(backupFilePath).entryList(BackupFilenameFilters, QDir::Files | QDir::CaseSensitive, QDir::NoSort);
  bool allRemoved = true;
  if (!backupFileNames.isEmpty()) {
    static const QRegExp reBackupFileTimestamp("^\\d{8}T\\d{6}");
    const QDateTime TooOld = QDateTime::currentDateTime().addDays(-d->optionsDialog->maxBackupFileAge());
    int nFilesRemoved = 0;
    foreach (QString backupFilename, backupFileNames) {
      if (reBackupFileTimestamp.indexIn(backupFilename) == 0) {
        const QDateTime fileTimestamp = QDateTime::fromString(reBackupFileTimestamp.cap(0), "yyyyMMddThhmmss");
        if (fileTimestamp < TooOld) {
          if (wipeFile(backupFilePath + QDir::separator() + backupFilename)) {
            emit backupFilesDeleted(++nFilesRemoved);
          }
          else {
            allRemoved = false;
          }
        }
      }
    }
  }
  emit backupFilesDeleted(allRemoved);
}


void MainWindow::removeOutdatedBackupFiles(void)
{
  Q_D(MainWindow);
  const QString &backupFilePath = QStandardPaths::writableLocation(QStandardPaths::DataLocation);
  const QStringList backupFileNames = QDir(backupFilePath).entryList(BackupFilenameFilters, QDir::Files | QDir::CaseSensitive, QDir::NoSort);
  if (!backupFileNames.isEmpty()) {
    d->backupFileDeletionFuture = QtConcurrent::run(this, &MainWindow::removeOutdatedBackupFilesThread);
  }
  else {
    ui->statusBar->showMessage(tr("There are no backup files present in %1.")
                               .arg(backupFilePath), 5000);
  }
}


void MainWindow::onBackupFilesRemoved(bool ok)
{
  Q_D(MainWindow);
  if (ok) {
    ui->statusBar->showMessage(tr("All of your backup files in %1 have been successfully removed.")
                               .arg(QStandardPaths::writableLocation(QStandardPaths::DataLocation)), 5000);
  }
  else {
    int rc = QMessageBox::warning(this,
                                  tr("Backup files remaining"),
                                  tr("Not all of your backup files in %1 have been successfully wiped. "
                                     "Shall I take you to the directory so that you can remove them manually?")
                                  .arg(QStandardPaths::writableLocation(QStandardPaths::DataLocation)),
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::Yes);
    if (rc == QMessageBox::Yes) {
      QDesktopServices::openUrl(QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::DataLocation)));
    }
  }
  writeBackupFile();
}


void MainWindow::onBackupFilesRemoved(int n)
{
  ui->statusBar->showMessage(tr("Deleted %1 outdated backup files.").arg(n), 3000);
}

void MainWindow::writeBackupFile(void)
{
  Q_D(MainWindow);
  const QString &backupFilePath = QStandardPaths::writableLocation(QStandardPaths::DataLocation);
  const QString &backupFilename = QString("%1/%2-%3-backup.txt")
      .arg(backupFilePath)
      .arg(QDateTime::currentDateTime().toString("yyyyMMddThhmmss"))
      .arg(AppName);
  if (QDir().mkpath(backupFilePath)) {
    _LOG(QString("Writing backup of settings to %1 ...").arg(backupFilename));
    QSettings backupSettings(backupFilename, QSettings::IniFormat);
    foreach (QString key, d->settings.allKeys()) {
      backupSettings.setValue(key, d->settings.value(key));
    }
    backupSettings.sync();
  }
}


void MainWindow::saveAllDomainDataToSettings(void)
{
  Q_D(MainWindow);
  // qDebug() << "MainWindow::saveAllDomainDataToSettings()";
  if (!d->masterKey.isEmpty()) {
    QByteArray cipher;
    {
      QMutexLocker locker(&d->keyGenerationMutex);
      try {
        d->keyGenerationFuture.waitForFinished();
        if (validCredentials()) {
          cipher = Crypter::encode(d->masterKey, d->IV, d->salt, d->kgk(), d->domains.toJson(), CompressionEnabled);
        }
        else {
          _LOG(QString("ERROR in MainWindow::saveAllDomainDataToSettings(): invalid credentials"));
        }
      }
      catch (CryptoPP::Exception &e) {
        qErrnoWarning((int)e.GetErrorType(), e.what());
        _LOG(QString("ERROR in MainWindow::saveAllDomainDataToSettings(): %1").arg(e.what()));
        return;
      }
    }
    if (!cipher.isEmpty()) {
      const QString &b64DomainData = QString::fromUtf8(cipher.toBase64());
      d->settings.setValue("sync/domains", b64DomainData);
      d->settings.sync();
      if (d->masterPasswordChangeStep == 0) {
        if (d->optionsDialog->writeBackups()) {
          writeBackupFile();
        }
        generateSaltKeyIV().waitForFinished();
      }
    }
  }
  else {
    _LOG("ERROR in MainWindow::saveAllDomainDataToSettings(): d->masterKey must not empty");
  }
}


bool MainWindow::restoreDomainDataFromSettings(void)
{
  Q_D(MainWindow);
  Q_ASSERT_X(!d->masterPassword.isEmpty(), "MainWindow::restoreDomainDataFromSettings()", "d->masterPassword must not be empty");
  QJsonDocument json;
  QStringList domainList;
  const QByteArray &domains = QByteArray::fromBase64(d->settings.value("sync/domains").toByteArray());
  if (!domains.isEmpty()) {
    QByteArray recovered;
    try {
      recovered = Crypter::decode(d->masterPassword.toUtf8(), domains, CompressionEnabled, d->KGK);
    }
    catch (CryptoPP::Exception &e) {
      wrongPasswordWarning((int)e.GetErrorType(), e.what());
      return false;
    }
    QJsonParseError parseError;
    json = QJsonDocument::fromJson(recovered, &parseError);
    if (parseError.error == QJsonParseError::NoError) {
      domainList = json.object().keys();
      ui->statusBar->showMessage(tr("Password accepted. Restored %1 domains.")
                                 .arg(domainList.count()), 5000);
    }
    else {
      QMessageBox::warning(this, tr("Bad data from sync server"),
                           tr("Decoding the data from the sync server failed: %1")
                           .arg(parseError.errorString()), QMessageBox::Ok);
    }
  }
  d->domains = DomainSettingsList::fromQJsonDocument(json);
  makeDomainComboBox();
  return true;
}


void MainWindow::saveSyncDataToSettings(void)
{
  Q_D(MainWindow);
  // qDebug() << "MainWindow::saveSyncDataToSettings()";
  QMutexLocker(&d->keyGenerationMutex);
  QVariantMap syncData;
  syncData["sync/server/root"] = d->optionsDialog->serverRootUrl();
  syncData["sync/server/username"] = d->optionsDialog->serverUsername();
  syncData["sync/server/password"] = d->optionsDialog->serverPassword();
  syncData["sync/server/rootCertificates"] = QString(d->optionsDialog->serverRootCertificate().toPem());
  syncData["sync/server/secure"] = d->optionsDialog->secure();
  syncData["sync/server/writeUrl"] = d->optionsDialog->writeUrl();
  syncData["sync/server/readUrl"] = d->optionsDialog->readUrl();
  syncData["sync/server/deleteUrl"] = d->optionsDialog->deleteUrl();
  syncData["sync/onStart"] = d->optionsDialog->syncOnStart();
  syncData["sync/filename"] = d->optionsDialog->syncFilename();
  syncData["sync/useFile"] = d->optionsDialog->useSyncFile();
  syncData["sync/useServer"] = d->optionsDialog->useSyncServer();
  QByteArray baCryptedData;
  try {
    d->keyGenerationFuture.waitForFinished();
    if (validCredentials()) {
      baCryptedData = Crypter::encode(d->masterKey, d->IV, d->salt, d->kgk(), QJsonDocument::fromVariant(syncData).toJson(QJsonDocument::Compact), CompressionEnabled);
    }
    else {
      _LOG(QString("ERROR in MainWindow::collectedSyncData(): invalid credentials"));
    }
  }
  catch (CryptoPP::Exception &e) {
    wrongPasswordWarning((int)e.GetErrorType(), e.what());
    _LOG(QString("ERROR in MainWindow::collectedSyncData(): %1").arg(e.what()));
  }
  d->settings.setValue("sync/param",baCryptedData.toBase64());
  d->settings.sync();
}


void MainWindow::saveSettings(void)
{
  Q_D(MainWindow);
  // qDebug() << "MainWindow::saveSettings()";
  _LOG("MainWindow::saveSettings()");
  saveSyncDataToSettings();
  saveAllDomainDataToSettings();
  saveUiSettings();
}


void MainWindow::saveUiSettings(void)
{
  Q_D(MainWindow);
  // qDebug() << "MainWindow::saveUiSettings()";
  // _LOG("MainWindow::saveUiSettings()");
  d->settings.setValue("mainwindow/geometry", saveGeometry());
  d->settings.setValue("mainwindow/language", d->language);
  d->settings.setValue("mainwindow/lastAttachFileDir", d->lastAttachFileDir);
  d->settings.setValue("mainwindow/lastSaveAttachmentDir", d->lastSaveAttachmentDir);
  d->settings.setValue("misc/optionsTabIndex", d->optionsDialog->activeTab());
  d->settings.setValue("misc/masterPasswordInvalidationTimeMins", d->optionsDialog->masterPasswordInvalidationTimeMins());
  d->settings.setValue("misc/maxPasswordLength", d->optionsDialog->maxPasswordLength());
  d->settings.setValue("misc/defaultPasswordLength", d->optionsDialog->defaultPasswordLength());
  d->settings.setValue("misc/defaultPBKDF2Iterations", d->optionsDialog->defaultIterations());
  d->settings.setValue("misc/saltLength", d->optionsDialog->saltLength());
  d->settings.setValue("misc/writeBackups", d->optionsDialog->writeBackups());
  d->settings.setValue("misc/autoDeleteBackupFiles", d->optionsDialog->autoDeleteBackupFiles());
  d->settings.setValue("misc/maxBackupFileAge", d->optionsDialog->maxBackupFileAge());
  d->settings.setValue("misc/maxAttachmentSizeKbyte", d->optionsDialog->maxAttachmentSizeKbyte());
  d->settings.setValue("misc/extensiveWipeout", d->optionsDialog->extensiveWipeout());
  d->settings.setValue("misc/passwordFile", d->optionsDialog->passwordFilename());
  d->settings.setValue("misc/moreSettingsExpanded", d->expandableGroupBox->expanded());
  d->settings.setValue("misc/loggingEnabled", d->optionsDialog->loggingEnabled());
  d->settings.sync();
}


void MainWindow::restoreUiSettings(void)
{
  Q_D(MainWindow);
  restoreGeometry(d->settings.value("mainwindow/geometry").toByteArray());
  d->language = d->settings.value("mainwindow/language", defaultLocale()).toString();
  d->lastAttachFileDir = d->settings.value("mainwindow/lastAttachFileDir").toString();
  d->lastSaveAttachmentDir = d->settings.value("mainwindow/lastSaveAttachmentDir").toString();
  d->optionsDialog->setActiveTab(d->settings.value("misc/optionsTabIndex", 0).toInt());
  d->optionsDialog->setMasterPasswordInvalidationTimeMins(d->settings.value("misc/masterPasswordInvalidationTimeMins", DefaultMasterPasswordInvalidationTimeMins).toInt());
  d->optionsDialog->setWriteBackups(d->settings.value("misc/writeBackups", true).toBool());
  d->optionsDialog->setPasswordFilename(d->settings.value("misc/passwordFile").toString());
  d->optionsDialog->setSaltLength(d->settings.value("misc/saltLength", DomainSettings::DefaultSaltLength).toInt());
  d->optionsDialog->setMaxPasswordLength(d->settings.value("misc/maxPasswordLength", Password::DefaultMaxLength).toInt());
  d->optionsDialog->setDefaultPasswordLength(d->settings.value("misc/defaultPasswordLength", DomainSettings::DefaultPasswordLength).toInt());
  d->optionsDialog->setDefaultIterations(d->settings.value("misc/defaultPBKDF2Iterations", DomainSettings::DefaultIterations).toInt());
  d->optionsDialog->setMaxBackupFileAge(d->settings.value("misc/maxBackupFileAge", 30).toInt());
  d->optionsDialog->setMaxAttachmentSizeKbyte(d->settings.value("misc/maxAttachmentSizeKbyte", 50).toInt());
  d->optionsDialog->setAutoDeleteBackupFiles(d->settings.value("misc/autoDeleteBackupFiles", true).toBool());
  d->optionsDialog->setExtensiveWipeout(d->settings.value("misc/extensiveWipeout", false).toBool());
  d->optionsDialog->setSyncFilename(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/" + AppName + ".bin");
  d->optionsDialog->setServerRootUrl(DefaultSyncServerRoot);
  d->optionsDialog->setServerUsername(DefaultSyncServerUsername);
  d->optionsDialog->setServerPassword(DefaultSyncServerPassword);
  d->optionsDialog->setReadUrl(DefaultSyncServerReadUrl);
  d->optionsDialog->setWriteUrl(DefaultSyncServerWriteUrl);
  d->optionsDialog->setDeleteUrl(DefaultSyncServerDeleteUrl);
  d->expandableGroupBox->setExpanded(d->settings.value("misc/moreSettingsExpanded", false).toBool());
  d->optionsDialog->setLoggingEnabled(d->settings.value("misc/loggingEnabled", false).toBool());
}


bool MainWindow::restoreSyncSettings(void)
{
  Q_D(MainWindow);
  QByteArray baCryptedData = QByteArray::fromBase64(d->settings.value("sync/param").toByteArray());
  if (!baCryptedData.isEmpty()) {
    QByteArray baSyncData;
    try {
      baSyncData = Crypter::decode(d->masterPassword.toUtf8(), baCryptedData, CompressionEnabled, d->KGK);
    }
    catch (CryptoPP::Exception &e) {
      wrongPasswordWarning((int)e.GetErrorType(), e.what());
      return false;
    }
    const QJsonDocument &jsonSyncData = QJsonDocument::fromJson(baSyncData);
    QVariantMap syncData = jsonSyncData.toVariant().toMap();
    d->optionsDialog->setSyncFilename(syncData["sync/filename"].toString());
    d->optionsDialog->setSyncOnStart(syncData["sync/onStart"].toBool());
    d->optionsDialog->setUseSyncFile(syncData["sync/useFile"].toBool());
    d->optionsDialog->setUseSyncServer(syncData["sync/useServer"].toBool());
    d->optionsDialog->setServerRootUrl(syncData["sync/server/root"].toString());
    d->optionsDialog->setWriteUrl(syncData["sync/server/writeUrl"].toString());
    d->optionsDialog->setReadUrl(syncData["sync/server/readUrl"].toString());
    d->optionsDialog->setDeleteUrl(syncData["sync/server/deleteUrl"].toString());
    d->optionsDialog->setServerCertificates(QSslCertificate::fromData(syncData["sync/server/rootCertificates"].toByteArray(), QSsl::Pem));
    d->optionsDialog->setSecure(syncData["sync/server/secure"].toBool());
    d->optionsDialog->setServerUsername(syncData["sync/server/username"].toString());
    d->optionsDialog->setServerPassword(syncData["sync/server/password"].toString());
  }
  Logger::instance().setEnabled(d->settings.value("misc/logger/enabled", true).toBool());
  _LOG("MainWindow::restoreSettings() finish.");
  return true;
}


bool MainWindow::restoreSettings(void)
{
  Q_D(MainWindow);
  return restoreSyncSettings();
}


#if HACKING_MODE_ENABLED
void MainWindow::hackLegacyPassword(void)
{
  Q_D(MainWindow);
  const QString &pwd = ui->legacyPasswordLineEdit->text();
  if (pwd.isEmpty()) {
    QMessageBox::information(this, tr("Cannot hack"), tr("No legacy password given. Cannot hack!"));
  }
  else {
    ui->tabWidget->setCurrentIndex(0);
    blockUpdatePassword();
    d->masterPasswordInvalidationTimer.stop();
    d->hackingMode = true;
    d->hackSalt.fill(0);
    d->hackPos = PositionTable(pwd);
    d->hackPermutations = d->hackPos.permutations();
    d->hackIterationDurationMs = 0;
    const QStringList &chrs = pwd.split("", QString::SkipEmptyParts).toSet().toList(); // keep this for backwards compatibility (Qt < 5.5)
    ui->usedCharactersPlainTextEdit->setPlainText(chrs.join(""));
    ui->legacyPasswordLineEdit->setReadOnly(true);
    ui->usedCharactersPlainTextEdit->setReadOnly(true);
    ui->renewSaltPushButton->setEnabled(false);
    ui->passwordLengthSpinBox->setValue(pwd.size());
    d->hackClock.restart();
    d->hackIterationClock.restart();
    unblockUpdatePassword();
    ui->saltBase64LineEdit->setText(d->hackSalt.toBase64());
  }
}
#endif


void MainWindow::onWriteFinished(QNetworkReply *reply)
{
  Q_D(MainWindow);
  ++d->counter;
  d->progressDialog->setValue(d->counter);
  if (reply->error() == QNetworkReply::NoError) {
    if (d->masterPasswordChangeStep > 0) {
      nextChangeMasterPasswordStep();
    }
    else {
      if (d->counter == d->maxCounter) {
        d->progressDialog->setText(tr("Sync to server finished."));
        if (d->doConvertLocalToLegacy && !d->optionsDialog->useSyncFile())
          warnAboutDifferingKGKs();
      }
    }
  }
  else {
    d->progressDialog->setText(tr("Writing to the server failed. Reason: %1").arg(reply->errorString()));
  }
  reply->close();
}


void MainWindow::cancelServerOperation(void)
{
  Q_D(MainWindow);
  if (d->readReply != Q_NULLPTR && d->readReply->isRunning()) {
    d->readReply->abort();
    ui->statusBar->showMessage(tr("Server read operation aborted."), 3000);
  }
  if (d->writeReply != Q_NULLPTR && d->writeReply->isRunning()) {
    d->writeReply->abort();
    ui->statusBar->showMessage(tr("Sync to server aborted."), 3000);
  }
}


void MainWindow::createEmptySyncFile(void)
{
  Q_D(MainWindow);
  QFile syncFile(d->optionsDialog->syncFilename());
  bool ok = syncFile.open(QIODevice::WriteOnly);
  if (!ok) {
    QMessageBox::warning(this, tr("Sync file creation error"),
                         tr("The sync file %1 cannot be created. Reason: %2")
                         .arg(d->optionsDialog->syncFilename())
                         .arg(syncFile.errorString()), QMessageBox::Ok);
    return;
  }
  QMutexLocker(&d->keyGenerationMutex);
  d->keyGenerationFuture.waitForFinished();
  QByteArray domains;
  try {
    if (validCredentials()) {
      domains = Crypter::encode(d->masterKey, d->IV, d->salt, d->kgk(), QByteArray("{}"), CompressionEnabled);
    }
    else {
      _LOG(QString("ERROR in MainWindow::createEmptySyncFile(): invalid credentials"));
    }
  }
  catch (CryptoPP::Exception &e) {
    _LOG(QString("ERROR in MainWindow::createEmptySyncFile(): %1").arg(e.what()));
    return;
  }
  if (!domains.isEmpty() && syncFile.isOpen()) {
    syncFile.write(domains);
    syncFile.close();
  }
}


void MainWindow::syncWithFile(void)
{
  Q_D(MainWindow);
  // qDebug() << "MainWindow::syncWithFile()";
  _LOG(QString("MainWindow::syncWithFile() %1").arg(d->optionsDialog->syncFilename()));
  QFile syncFile(d->optionsDialog->syncFilename());
  bool ok = syncFile.open(QIODevice::ReadOnly);
  if (!ok) {
    QMessageBox::warning(this, tr("Sync file read error"),
                         tr("The sync file %1 cannot be opened for reading. Reason: %2")
                         .arg(d->optionsDialog->syncFilename()).arg(syncFile.errorString()), QMessageBox::Ok);
  }
  QByteArray domains = syncFile.readAll();
  syncFile.close();
  syncWith(SyncPeerFile, domains);
}


void MainWindow::beginSyncWithServer(void)
{
  Q_D(MainWindow);
  d->progressDialog->setText(tr("Reading from server ..."));
  QUrl serverUrl = QUrl(d->optionsDialog->serverRootUrl() + d->optionsDialog->readUrl());
  _LOG(QString("MainWindow::beginSyncWithServer() %1").arg(serverUrl.toString()));
  QNetworkRequest req(serverUrl);
  req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
  req.setHeader(QNetworkRequest::UserAgentHeader, AppUserAgent);
  req.setRawHeader("Authorization", d->optionsDialog->httpBasicAuthenticationString());
  req.setSslConfiguration(d->sslConf);
  d->readReply = d->readNAM.post(req, QByteArray());
}


void MainWindow::onSync(void)
{
  Q_D(MainWindow);
  restartInvalidationTimer();
  d->domainSettingsBeforceSync = d->domains.at(ui->domainsComboBox->currentText());
  if (d->optionsDialog->useSyncFile() && !d->optionsDialog->syncFilename().isEmpty()) {
    ui->statusBar->showMessage(tr("Syncing with file ..."));
    QFileInfo fi(d->optionsDialog->syncFilename());
    if (!fi.isFile()) {
      createEmptySyncFile();
    }
    if (fi.isFile() && fi.isReadable()) {
      syncWithFile();
    }
    else {
      QMessageBox::warning(this,
                           tr("Sync file read error"),
                           tr("The sync file %1 cannot be opened for reading.")
                           .arg(d->optionsDialog->syncFilename()), QMessageBox::Ok);
    }
    if (d->doConvertLocalToLegacy && !d->optionsDialog->useSyncServer()) {
      warnAboutDifferingKGKs();
    }
  }
  if (d->optionsDialog->useSyncServer()) {
    if (d->masterPasswordChangeStep == 0) {
      d->progressDialog->show();
      d->progressDialog->raise();
      d->counter = 0;
      d->maxCounter = 1;
      d->progressDialog->setRange(0, d->maxCounter);
      d->progressDialog->setValue(d->counter);
    }
    beginSyncWithServer();
  }
}


QByteArray MainWindow::cryptedRemoteDomains(void)
{
  Q_D(MainWindow);
  QMutexLocker(&d->keyGenerationMutex);
  QByteArray cipher;
  try {
    d->keyGenerationFuture.waitForFinished();
    if (validCredentials()) {
      cipher = Crypter::encode(d->masterKey, d->IV, d->salt, d->kgk(), d->remoteDomains.toJson(), CompressionEnabled);
    }
    else {
      _LOG(QString("ERROR in MainWindow::cryptedRemoteDomains(): invalid credentials"));
    }
  }
  catch (CryptoPP::Exception &e) {
    wrongPasswordWarning((int)e.GetErrorType(), e.what());
  }
  return cipher;
}


void MainWindow::warnAboutDifferingKGKs(void)
{
  QMessageBox::information(this,
                           tr("KGKs differ"),
                           tr("The remote key generation key (KGK) differs from the local one. "
                              "You probably began entering domain settings on this computer without syncing beforehand. "
                              "The local settings have be converted so that generated passwords became legacy passwords. "
                              "All settings have been kept, none of your work is lost."));
}


void MainWindow::syncWith(SyncPeer syncPeer, const QByteArray &remoteDomainsEncoded)
{
  Q_D(MainWindow);
  // qDebug() << "MainWindow::syncWith(" << syncPeer << ")";
  QJsonDocument remoteJSON;
  d->doConvertLocalToLegacy = false;
  if (!remoteDomainsEncoded.isEmpty()) {
    QByteArray baDomains;
    bool ok = true;
    try {
      SecureByteArray KGK;
      baDomains = Crypter::decode(d->masterPassword.toUtf8(), remoteDomainsEncoded, CompressionEnabled, KGK);
      if (d->KGK != KGK) {
        d->doConvertLocalToLegacy = !d->domains.isEmpty();
        d->KGK = KGK;
      }
    }
    catch (CryptoPP::Exception &e) {
      ok = false;
      if (d->masterPasswordChangeStep == 0) {
        wrongPasswordWarning((int)e.GetErrorType(), e.what());
        return;
      }
    }
    if (!ok) { // fall back to new password
      try {
        SecureByteArray KGK;
        baDomains = Crypter::decode(d->changeMasterPasswordDialog->newPassword().toUtf8(), remoteDomainsEncoded, CompressionEnabled, KGK);
        if (d->KGK != KGK && !d->domains.isEmpty()) {
          d->doConvertLocalToLegacy = true;
          d->KGK = KGK;
        }
      }
      catch (CryptoPP::Exception &e) {
        wrongPasswordWarning((int)e.GetErrorType(), e.what());
        return;
      }
    }
    if (!baDomains.isEmpty()) {
      QJsonParseError parseError;
      remoteJSON = QJsonDocument::fromJson(baDomains, &parseError);
      if (parseError.error != QJsonParseError::NoError) {
        QMessageBox::warning(this, tr("Bad data from sync peer"),
                             tr("Decoding the data from the sync peer failed: %1")
                             .arg(parseError.errorString()), QMessageBox::Ok);
      }
    }
  }

  d->domains.setDirty(false);
  d->remoteDomains = DomainSettingsList::fromQJsonDocument(remoteJSON);
  mergeLocalAndRemoteData();

  if (d->remoteDomains.isDirty()) {
    writeToRemote(syncPeer);
  }

  if (d->domains.isDirty()) {
    saveAllDomainDataToSettings();
    restoreDomainDataFromSettings();
    d->domains.setDirty(false);
  }

  copyDomainSettingsToGUI(d->domainSettingsBeforceSync);
}


void MainWindow::shrink(void)
{
  const QSize &newSize = QSize(width(), 0);
  resize(newSize);
}


void MainWindow::onExpandableCheckBoxStateChanged(void)
{
  Q_D(MainWindow);
  if (!d->expandableGroupBox->expanded()) {
    QTimer::singleShot(10, this, SLOT(shrink()));
  }
}


void MainWindow::onTabChanged(int idx)
{
  Q_D(MainWindow);
  if (idx == TabLegacyPassword) {
    d->expandableGroupBoxLastExpanded = d->expandableGroupBox->expanded();
    if (d->expandableGroupBoxLastExpanded)
      d->expandableGroupBox->collapse();
  }
  else {
    if (d->expandableGroupBoxLastExpanded)
      d->expandableGroupBox->expand();
  }
}


void MainWindow::convertToLegacyPassword(DomainSettings &ds)
{
  Q_D(MainWindow);
  if (ds.legacyPassword.isEmpty()) {
    Password pwd(ds);
    Q_ASSERT_X(!d->masterPassword.isEmpty(), "MainWindow::convertToLegacyPassword()", "d->masterPassword must not be empty");
    if (d->masterPassword.isEmpty()) {
      qWarning() << "Error in MainWindow::convertToLegacyPassword(): d->masterPassword must not be empty";
      return;
    }
    pwd.generate(d->masterPassword.toUtf8());
    ds.legacyPassword = pwd.password();
  }
}


void MainWindow::mergeLocalAndRemoteData(void)
{
  Q_D(MainWindow);
  QStringList allDomainNames = d->remoteDomains.keys() + d->domains.keys();
  allDomainNames.removeDuplicates();
  foreach(QString domainName, allDomainNames) {
    const DomainSettings &remoteDomainSetting = d->remoteDomains.at(domainName);
    DomainSettings localDomainSetting = d->domains.at(domainName);
    if (!localDomainSetting.isEmpty() && !remoteDomainSetting.isEmpty()) {
      if (remoteDomainSetting.modifiedDate > localDomainSetting.modifiedDate) {
        d->domains.updateWith(remoteDomainSetting);
      }
      else if (remoteDomainSetting.modifiedDate < localDomainSetting.modifiedDate) {
        if (d->doConvertLocalToLegacy && !localDomainSetting.deleted) {
          convertToLegacyPassword(localDomainSetting);
          localDomainSetting.domainName = selectAlternativeDomainNameFor(domainName, d->domains.keys());
        }
        d->remoteDomains.updateWith(localDomainSetting);
      }
    }
    else if (remoteDomainSetting.isEmpty()) {
      if (!localDomainSetting.deleted) {
        if (d->doConvertLocalToLegacy) {
          convertToLegacyPassword(localDomainSetting);
        }
        d->remoteDomains.updateWith(localDomainSetting);
      }
      else {
        d->domains.remove(domainName);
      }
    }
    else {
      d->domains.updateWith(remoteDomainSetting);
    }
  }
}


void MainWindow::writeToRemote(SyncPeer syncPeer)
{
  Q_D(MainWindow);
  const QByteArray &cipher = cryptedRemoteDomains();
  if (!cipher.isEmpty()) {
    if ((syncPeer & SyncPeerFile) == SyncPeerFile && d->optionsDialog->syncToFileEnabled()) {
      writeToSyncFile(cipher);
    }
    if ((syncPeer & SyncPeerServer) == SyncPeerServer && d->optionsDialog->syncToServerEnabled()) {
      sendToSyncServer(cipher);
    }
  }
  else {
    // TODO: catch encryption error
  }
}


void MainWindow::writeToSyncFile(const QByteArray &cipher)
{
  Q_D(MainWindow);
  if (d->optionsDialog->syncToFileEnabled()) {
    QFile syncFile(d->optionsDialog->syncFilename());
    syncFile.open(QIODevice::WriteOnly);
    const qint64 bytesWritten = syncFile.write(cipher);
    syncFile.close();
    if (bytesWritten < 0) {
      QMessageBox::warning(this, tr("Sync file write error"), tr("Writing to your sync file %1 failed: %2")
                           .arg(d->optionsDialog->syncFilename())
                           .arg(syncFile.errorString()), QMessageBox::Ok);
    }
  }
}


void MainWindow::sendToSyncServer(const QByteArray &cipher)
{
  Q_D(MainWindow);
  if (d->masterPasswordChangeStep == 0) {
    d->counter = 0;
    d->maxCounter = 1;
    d->progressDialog->setText(tr("Sending data to server ..."));
    d->progressDialog->setRange(0, d->maxCounter);
    d->progressDialog->setValue(0);
    d->progressDialog->show();
  }
  QUrlQuery params;
  // XXX: Wouldn't QByteArray::Base64UrlEncoding be better?
  params.addQueryItem("data", cipher.toBase64(QByteArray::Base64Encoding));
  const QByteArray &data = params.query().toUtf8();
  QNetworkRequest req(QUrl(d->optionsDialog->serverRootUrl() + d->optionsDialog->writeUrl()));
  req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
  req.setHeader(QNetworkRequest::ContentLengthHeader, data.size());
  req.setHeader(QNetworkRequest::UserAgentHeader, AppUserAgent);
  req.setRawHeader("Authorization", d->optionsDialog->httpBasicAuthenticationString());
  req.setSslConfiguration(d->sslConf);
  d->writeReply = d->writeNAM.post(req, data);
}


void MainWindow::onForcedPush(void)
{
  Q_D(MainWindow);
  QByteArray cipher;
  {
    QMutexLocker(&d->keyGenerationMutex);
    try {
      d->keyGenerationFuture.waitForFinished();
      if (validCredentials()) {
        cipher = Crypter::encode(d->masterKey, d->IV, d->salt, d->kgk(), d->domains.toJson(), CompressionEnabled);
      }
      else {
        _LOG("ERROR in MainWindow::onForcedPush(): invalid credentials");
      }
    }
    catch (CryptoPP::Exception &e) {
      wrongPasswordWarning((int)e.GetErrorType(), e.what());
      return;
    }
  }
  if (!cipher.isEmpty()) {
    sendToSyncServer(cipher);
  }
}


void MainWindow::onDomainSelected(QString domain)
{
  Q_D(MainWindow);
  _LOG(QString("MainWindow::onDomainSelected(\"%1\") d->lastCleanDomainSettings.domainName = \"%2\", SENDER = %3")
       .arg(domain)
       .arg(d->lastCleanDomainSettings.domainName)
       .arg((sender() != Q_NULLPTR ? sender()->objectName() : "NONE")));
  if (!domainComboboxContains(domain))
    return;
  if (sender() == Q_NULLPTR)
    return;
  if (domain == d->lastCleanDomainSettings.domainName)
    return;
  if (d->parameterSetDirty) {
    ui->domainsComboBox->blockSignals(true);
    ui->domainsComboBox->setCurrentText(d->lastCleanDomainSettings.domainName);
    ui->domainsComboBox->blockSignals(false);
    QMessageBox::StandardButton button = saveYesNoCancel();
    switch (button) {
    case QMessageBox::Yes:
      saveCurrentDomainSettings();
      break;
    case QMessageBox::No:
      break;
    case QMessageBox::Cancel:
      return;
      break;
    default:
      break;
    }
  }
  d->lastCleanDomainSettings = d->domains.at(domain);
  // qDebug() << d->lastCleanDomainSettings;
  copyDomainSettingsToGUI(d->lastCleanDomainSettings);
  ui->generatedPasswordLineEdit->setEchoMode(QLineEdit::Password);
  setDirty(false);
}


void MainWindow::onDomainTextChanged(const QString &domain)
{
  Q_D(MainWindow);
  _LOG(QString("MainWindow::onDomainTextChanged(\"%1\") d->lastCleanDomainSettings.domainName = \"%2\"")
       .arg(domain)
       .arg(d->lastCleanDomainSettings.domainName));
  int idx = findDomainInComboBox(domain);
  if (idx == NotFound) {
    if (!d->lastCleanDomainSettings.isEmpty()) {
      ui->tabWidget->setCurrentIndex(TabGeneratedPassword);
      resetAllFieldsExceptDomainComboBox();
    }
    ui->generatedPasswordLineEdit->setEchoMode(QLineEdit::Normal);
    setTemplate();
    updatePassword();
    d->lastCleanDomainSettings.clear();
    ui->tabWidget->setCurrentIndex(TabGeneratedPassword);
  }
}


void MainWindow::onEasySelectorValuesChanged(int passwordLength, int complexityValue)
{
  Q_D(MainWindow);
  Q_UNUSED(passwordLength);
  applyComplexity(complexityValue);
  setTemplate();
  d->password.setDomainSettings(collectedDomainSettings());
  const SecureString &pwd = d->password.remix();
  ui->generatedPasswordLineEdit->setText(pwd);
  ui->passwordLengthLabel->setText(tr("(%1 characters)").arg(passwordLength));
  d->pwdLabelOpacityEffect->setOpacity(pwd.isEmpty() ? 0.5 : 1.0);
  setDirty(true);
  restartInvalidationTimer();
}


static const QString JSONFileExtension = QObject::tr("JSON file (*.json *.txt)");

void MainWindow::onExportAllDomainSettingAsJSON(void)
{
  Q_D(MainWindow);
  QString filename =
      QFileDialog::getSaveFileName(this,
                                   tr("Export all domain settings as JSON"),
                                   QString(),
                                   JSONFileExtension);
  if (!filename.isEmpty()) {
    QFile f(filename);
    f.open(QIODevice::Truncate | QIODevice::WriteOnly);
    if (f.isOpen()) {
      QByteArray data = d->domains.toJsonDocument().toJson(QJsonDocument::Indented);
      f.write(data);
      f.close();
    }
   }
}


struct DomainSettingsToTextConverter
{
  explicit DomainSettingsToTextConverter(const SecureByteArray &kgk)
    : kgk(kgk)
  { /* ... */ }
  typedef SecureByteArray result_type;
  SecureByteArray kgk;
  SecureByteArray operator()(const DomainSettings &ds)
  {
    SecureByteArray data;
    if (!ds.deleted && !ds.expired()) {
      SecureString pwd = ds.legacyPassword;
      if (pwd.isEmpty()) {
        Password gpwd(ds);
        gpwd.generate(kgk);
        pwd = gpwd.password();
      }
      if (!pwd.isEmpty()) {
        QString notes = ds.notes;
        notes.replace("\\", "\\\\");
        notes.replace("\n", "\\n");
        data = SecureString("[%1]\n"
                            "pwd = %2\n")
            .arg(ds.domainName)
            .arg(pwd)
            .toUtf8();
        if (!ds.url.isEmpty()) {
          data.append(QString("url = %1\n").arg(ds.url).toUtf8());
        }
        if (!ds.userName.isEmpty()) {
          data.append(QString("user = %1\n").arg(ds.userName).toUtf8());
        }
        if (!notes.isEmpty()) {
          data.append(SecureString("notes = %1\n").arg(notes).toUtf8());
        }
        if (!ds.groupHierarchy.isEmpty()) {
          data.append(QString("group = %1\n").arg(ds.groupHierarchy).toUtf8());
        }
      }
    }
    return data;
  }
};


static const QString LoginDataFileExtension = QObject::tr("Login data file (*.txt *.sesam)");

void MainWindow::onExportAllLoginDataAsClearText(void)
{
  Q_D(MainWindow);
  QString filename =
      QFileDialog::getSaveFileName(this,
                                   tr("Export all login data as clear text"),
                                   QString(),
                                   LoginDataFileExtension);
  if (!filename.isEmpty()) {
    QProgressDialog progressDialog(this);
    progressDialog.setLabelText(tr("Exporting logins\nin %1 thread%2 ...")
                                .arg(QThread::idealThreadCount())
                                .arg(QThread::idealThreadCount() == 1 ? "" : tr("s")));
    progressDialog.show();
    QFutureWatcher<SecureByteArray> futureWatcher;
    QObject::connect(&futureWatcher, SIGNAL(finished()), &progressDialog, SLOT(reset()));
    QObject::connect(&progressDialog, SIGNAL(canceled()), &futureWatcher, SLOT(cancel()));
    QObject::connect(&futureWatcher, SIGNAL(progressRangeChanged(int, int)), &progressDialog, SLOT(setRange(int, int)));
    QObject::connect(&futureWatcher, SIGNAL(progressValueChanged(int)), &progressDialog, SLOT(setValue(int)));
    QFuture<SecureByteArray> future = QtConcurrent::mappedReduced<SecureByteArray>(
          d->domains,
          DomainSettingsToTextConverter(d->KGK),
          [](SecureByteArray &all, const SecureByteArray &intermediate)
          {
            if (!intermediate.isEmpty()) {
              all.append(intermediate).append("\n");
            }
          },
          QtConcurrent::OrderedReduce);
    futureWatcher.setFuture(future);
    progressDialog.exec();
    futureWatcher.waitForFinished();
    if (!futureWatcher.future().isCanceled()) {
      QFile outFile(filename);
      bool ok = outFile.open(QIODevice::Truncate | QIODevice::WriteOnly);
      if (ok) {
        outFile.write(future.result());
        outFile.close();
      }
      QMessageBox::information(this, tr("All login data exported"), tr("Successfully exported %1 logins.").arg(d->domains.count()));
    }
  }
}


QImage MainWindow::currentDomainSettings2QRCode(void) const
{
  static const int ModuleSize = 10;
  static const int Margin = ModuleSize;
  DomainSettings ds = collectedDomainSettings();
  QString str = QString("%1\n%2\n%3\n%4")
      .arg(ds.domainName)
      .arg(ds.url)
      .arg(ds.userName)
      .arg(ui->generatedPasswordLineEdit->text());
  QRcode *qrcode = QRcode_encodeString8bit(str.toStdString().c_str(), 0, QR_ECLEVEL_L);
  const int sz = qrcode->width * ModuleSize + 2 * Margin;
  QPixmap qr(sz, sz);
  QPainter p(&qr);
  p.fillRect(qr.rect(), Qt::white);
  p.setPen(QPen(Qt::transparent, 0));
  p.translate(QPoint(Margin, Margin));
  unsigned char *m = qrcode->data;
  for (int y = 0; y < qrcode->width; ++y) {
    unsigned char *row = m + y * qrcode->width;
    int yy = y * ModuleSize;
    for (int x = 0; x < qrcode->width; ++x) {
      p.setBrush(((row[x] & 0x1) == 0x1) ? Qt::black : Qt::white);
      p.drawRect(x * ModuleSize, yy, ModuleSize, ModuleSize);
    }
  }
  p.end();
  QRcode_free(qrcode);
  return qr.toImage();
}


bool MainWindow::validCredentials(void) const
{
  return !d_ptr->masterKey.isEmpty() && !d_ptr->IV.isEmpty();
}


static const QString QRCodeFileExtension = QObject::tr("QR code file (*.png)");

void MainWindow::onExportCurrentSettingsAsQRCode(void)
{
  Q_D(MainWindow);
  QString filename = QFileDialog::getSaveFileName(this,
                                                  tr("Export current settings as QR code"),
                                                  QString(),
                                                  QRCodeFileExtension);
  if (!filename.isEmpty()) {
    QImage qrcode = currentDomainSettings2QRCode();
    qrcode.save(filename);
  }
}


void MainWindow::onPasswordTemplateChanged(const QString &templ)
{
  Q_D(MainWindow);
  // qDebug() << "MainWindow::onPasswordTemplateChanged(" << templ << ")";
  applyTemplateStringToGUI(templ.toUtf8());
}


void MainWindow::masterPasswordInvalidationTimeMinsChanged(int timeoutMins)
{
  Q_D(MainWindow);
  if (timeoutMins > 0 && !d->parameterSetDirty) {
    d->countdownWidget->start(1000 * timeoutMins * 60);
  }
  else {
    d->countdownWidget->stop();
  }
}


void MainWindow::onShuffleUsername(void)
{
  Q_D(MainWindow);
  QString username;
  const int N = Password::LowerChars.length();
  for (int i = 0; i < 8; ++i) {
    const unsigned int r = static_cast<unsigned int>(qrand());
    const QChar &ch = Password::LowerChars.at(r % N);
    username.append(ch);
  }
  ui->userLineEdit->blockSignals(true);
  ui->userLineEdit->setText(username);
  ui->userLineEdit->blockSignals(false);
}


void MainWindow::onNewDomain(void)
{
  Q_D(MainWindow);
  if (!d->parameterSetDirty) {
    resetAllFields();
  }
  else {
    QMessageBox::StandardButton button =
        QMessageBox::question(this,
                              tr("Really create new domain?"),
                              tr("You selected to add a new domain but the current settings have been altered. "
                                 "Do you want to save the settings before continuing?"),
                              QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
                              QMessageBox::Yes);
    switch (button) {
    case QMessageBox::Yes:
      saveCurrentDomainSettings();
      // fall-through
    case QMessageBox::No:
      resetAllFields();
      break;
    case QMessageBox::Cancel:
      break;
    default:
      qWarning() << "Oops! This should not have happened :-(";
      break;
    }
  }
}


void MainWindow::onRevert(void)
{
  Q_D(MainWindow);
  // qDebug() << "MainWindow::onRevert()" << d->lastCleanDomainSettings.domainName;
  d->interactionSemaphore.acquire();
  QMessageBox::StandardButton button = QMessageBox::question(
        this,
        tr("Revert settings?"),
        tr("Do you really want to revert the settings?"),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Yes);
  d->interactionSemaphore.release();
  switch (button) {
  case QMessageBox::Yes:
    copyDomainSettingsToGUI(d->lastCleanDomainSettings);
    setDirty(false);
    break;
  case QMessageBox::Cancel:
    // fall-through
  default:
    break;
  }
}


void MainWindow::updateWindowTitle(void)
{
  Q_D(MainWindow);
  bool dirty = d->parameterSetDirty && !ui->domainsComboBox->currentText().isEmpty();
  setWindowTitle(QString("%1 %2 %3")
                 .arg(AppName)
                 .arg(dirty ? "*" : "")
                 .arg(isPortable() ? " - PORTABLE " : ""));
}


void MainWindow::clearClipboard(void)
{
  QApplication::clipboard()->clear();
  ui->statusBar->showMessage(tr("Clipboard cleared."), 3000);
}


void MainWindow::enterMasterPassword(void)
{
  Q_D(MainWindow);
  hide();
  d->optionsDialog->hide();
  d->masterPasswordDialog->setRepeatPassword(d->settings.value("mainwindow/masterPasswordEntered", false).toBool() == false);
  d->masterPasswordDialog->show();
  d->masterPasswordDialog->raise();
}


void MainWindow::onMasterPasswordEntered(void)
{
  Q_D(MainWindow);
  bool ok = true;
  qsrand(static_cast<uint>(QDateTime::currentDateTime().toMSecsSinceEpoch()));
  const QString masterPwd = d->masterPasswordDialog->masterPassword();
  const bool repeatedPasswordEntry = d->masterPasswordDialog->repeatedPasswordEntry();
  if (!masterPwd.isEmpty()) {
    d->masterPassword = masterPwd;
    ok = restoreSettings();
    if (ok) {
      createLanguageMenu();
      ok = restoreDomainDataFromSettings();
      if (ok) {
        generateSaltKeyIV().waitForFinished();
        d->settings.setValue("mainwindow/masterPasswordEntered", true);
        d->settings.sync();
        ui->domainsComboBox->setCurrentText(d->lastDomainBeforeLock);
        ui->domainsComboBox->setFocus();
        d->masterPasswordDialog->hide();
        show();
        if (d->optionsDialog->autoDeleteBackupFiles()) {
          removeOutdatedBackupFiles();
        }
        if (d->optionsDialog->syncOnStart()) {
          onSync();
        }
        else if (repeatedPasswordEntry) {
          int rc = QMessageBox::warning(this,
                               tr("Sync now!"),
                               tr("You've started %1 for the first time on this computer. "
                                  "If you're using a sync server or file, please go to the "
                                  "Options dialog, enter your sync settings there, and then do a sync. "
                                  "If you don't follow this advice you may encounter problems later on. "
                                  "Click OK to open the Options dialog now.").arg(AppName),
                               QMessageBox::Ok | QMessageBox::Ignore);
          if (rc == QMessageBox::Ok) {
            showOptionsDialog();
          }
        }
        restartInvalidationTimer();
      }
    }
  }
  if (!ok ) {
    enterMasterPassword();
  }
}


void MainWindow::onMasterPasswordClosing(void)
{
//  qDebug() << "MainWindow::onMasterPasswordClosing()";
  close();
}


void MainWindow::clearAllSettings(void)
{
  Q_D(MainWindow);
  int button = QMessageBox::warning(
        this,
        tr("%1 - Really clear all settings?").arg(AppName),
        tr("You have chosen to delete all of your settings, "
           "i.e. your application settings and all of your domain settings. "
           "After deletion you'll have to start from scratch. "
           "Do you really want to do that?"), QMessageBox::Yes, QMessageBox::No);
  if (button == QMessageBox::Yes) {
    resetAllFields();
    d->masterPasswordDialog->setRepeatPassword(true);
    ui->domainsComboBox->clear();
    d->settings.setValue("mainwindow/masterPasswordEntered", false);
    d->settings.remove("sync");
    d->settings.sync();
    if (d->optionsDialog->useSyncFile() && !d->optionsDialog->syncFilename().isEmpty()) {
      QFileInfo fi(d->optionsDialog->syncFilename());
      if (fi.isWritable()) {
        QFile(d->optionsDialog->syncFilename()).remove();
      }
    }
    if (d->optionsDialog->useSyncServer() && !d->optionsDialog->deleteUrl().isEmpty()) {
      QNetworkRequest req(QUrl(d->optionsDialog->serverRootUrl() + d->optionsDialog->deleteUrl()));
      req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
      req.setHeader(QNetworkRequest::UserAgentHeader, AppUserAgent);
      req.setRawHeader("Authorization", d->optionsDialog->httpBasicAuthenticationString());
      req.setSslConfiguration(d->sslConf);
      d->deleteReply = d->deleteNAM.post(req, QByteArray());
    }
    d->lastDomainBeforeLock.clear();
    invalidateMasterPassword(true);
  }
}


void MainWindow::wrongPasswordWarning(int errCode, QString errMsg)
{
  QMessageBox::critical(
        this,
        tr("%1 - Decryption error").arg(AppName),
        tr("An error occured while decrypting your data (#%1, %2). "
           "Maybe you entered a wrong password. "
           "Please enter the correct password!").arg(errCode).arg(errMsg),
        QMessageBox::Retry);
  enterMasterPassword();
}


void MainWindow::invalidateMasterPassword(bool reenter)
{
  Q_D(MainWindow);
  // qDebug() << "MainWindow::invalidatePassword()";
  SecureErase(d->masterPassword);
  d->masterPasswordDialog->invalidatePassword();
  d->KGK.invalidate();
  d->masterKey.invalidate();
  if (reenter) {
    enterMasterPassword();
  }
}


void MainWindow::lockApplication(void)
{
  Q_D(MainWindow);
  // qDebug() << "MainWindow::lockApplication() triggered by" << (sender() == Q_NULLPTR ? sender()->objectName() : "NONE");
  _LOG("MainWindow::lockApplication()");
  if (d->interactionSemaphore.available() == 0) {
    restartInvalidationTimer();
    return;
  }
  if (d->parameterSetDirty) {
    QMessageBox::information(this,
                             tr("Cannot lock due to pending changes"),
                             tr("You've made changes to the current domain settings. "
                                "The application cannot be locked unless the changes have been saved."),
                             QMessageBox::Ok,
                             QMessageBox::Ok);
    restartInvalidationTimer();
    return;
  }
  d->lastDomainBeforeLock = ui->domainsComboBox->currentText();
  saveSettings();
  invalidateMasterPassword(true);
}


void MainWindow::sslErrorsOccured(QNetworkReply *reply, const QList<QSslError> &errors)
{
  Q_UNUSED(reply);
  foreach (QSslError error, errors) {
    qWarning() << "SSL error occured: " << int(error.error()) << error.errorString();
  }
}


void MainWindow::onDeleteFinished(QNetworkReply *reply)
{
  Q_D(MainWindow);
  if (reply->error() == QNetworkReply::NoError) {
    const QByteArray &res = reply->readAll();
    QJsonParseError parseError;
    const QJsonDocument &json = QJsonDocument::fromJson(res, &parseError);
    if (parseError.error == QJsonParseError::NoError) {
      QVariantMap map = json.toVariant().toMap();
      if (map["status"].toString() == "ok") {
        QMessageBox::information(
              this,
              tr("Deletion on server finished"),
              tr("Your domain settings have been successfully deleted on the sync server"));
      }
      else {
        QMessageBox::warning(
              this,
              tr("Deletion on server failed"),
              tr("The deletion of your domain settings on the server failed: %1").arg(map["error"].toString()));
      }
    }
  }
  else {
    QMessageBox::warning(
          this,
          tr("Deletion on server failed"),
          tr("The deletion of your domain settings on the server failed: %1").arg(reply->errorString()));
  }
  reply->close();
}


void MainWindow::onReadFinished(QNetworkReply *reply)
{
  Q_D(MainWindow);
  ++d->counter;
  d->progressDialog->setValue(d->counter);

  if (reply->error() == QNetworkReply::NoError) {
    const QByteArray &res = reply->readAll();
    d->progressDialog->setText(tr("Reading from server finished."));
    QJsonParseError parseError;
    const QJsonDocument &json = QJsonDocument::fromJson(res, &parseError);
    if (parseError.error == QJsonParseError::NoError) {
      QVariantMap map = json.toVariant().toMap();
      if (map["status"].toString() == "ok") {
        QByteArray baDomains = QByteArray::fromBase64(map["result"].toByteArray());
        syncWith(SyncPeerServer, baDomains);
      }
      else {
        d->progressDialog->setText(tr("Reading from the sync server failed. Status: %1 - Error: %2").arg(map["status"].toString()).arg(map["error"].toString()));
      }
      if (d->masterPasswordChangeStep > 0) {
        nextChangeMasterPasswordStep();
      }
    }
    else {
      d->progressDialog->setText(tr("Decoding the data from the sync server failed: %1").arg(parseError.errorString()));
    }
  }
  else {
    d->progressDialog->setText(tr("Critical Network Error: %1").arg(reply->errorString()));
  }
  reply->close();
}


void MainWindow::about(void)
{
  QMessageBox::about(
        this, tr("About %1 %2").arg(AppName).arg(AppVersion),
        tr("<p><b>%1</b> is a domain specific password generator. "
           "See <a href=\"%2\" title=\"%1 project homepage\">%2</a> for more info.</p>"
           "<p>This program is free software: you can redistribute it and/or modify "
           "it under the terms of the GNU General Public License as published by "
           "the Free Software Foundation, either version 3 of the License, or "
           "(at your option) any later version.</p>"
           "<p>This program is distributed in the hope that it will be useful, "
           "but WITHOUT ANY WARRANTY; without even the implied warranty of "
           "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the "
           "GNU General Public License for more details.</p>"
           "You should have received a copy of the GNU General Public License "
           "along with this program. "
           "If not, see <a href=\"http://www.gnu.org/licenses/gpl-3.0\">http://www.gnu.org/licenses</a>.</p>"
           "<p>Copyright &copy; 2015 %3 &lt;%4&gt;, Heise Medien GmbH &amp; Co. KG.</p>"
           "<p>"
           " This program uses the Crypto++ library and libqrencode by Kentaro Fukuchi. "
           " Crypto++ is licensed under the Boost Software License, Version 1.0. "
           " libqrencode is licensed under the GNU Lesser General Public License 2.1 or later."
           "</p>"
           )
        .arg(AppName).arg(AppURL).arg(AppAuthor).arg(AppAuthorMail));
}


void MainWindow::aboutQt(void)
{
  QMessageBox::aboutQt(this);
}


void MainWindow::executeAttachmentContextMenu(QEvent *event)
{
  Q_D(MainWindow);
  const QContextMenuEvent *const cmEvent = reinterpret_cast<QContextMenuEvent*>(event);
  const int row = ui->attachmentTableWidget->rowAt(cmEvent->pos().y()
                                                   - ui->attachmentTableWidget->horizontalHeader()->height());
  const QTableWidgetItem *const item = ui->attachmentTableWidget->item(row, 0);
  const bool additionalMenuItemsVisible = (item != Q_NULLPTR);
  d->actionSaveAttachment->setVisible(additionalMenuItemsVisible);
  d->actionDeleteAttachment->setVisible(additionalMenuItemsVisible);
  const QAction *const selectedAction = d->attachmentsContextMenu->exec(cmEvent->globalPos());
  if (selectedAction == d->actionAttachFile) {
    onAttachFile();
  }
  else if (selectedAction == d->actionSaveAttachment && additionalMenuItemsVisible) {
    saveAttachmentAs(item);
  }
  else if (selectedAction == d->actionDeleteAttachment && additionalMenuItemsVisible) {
    QList<int> rowsToBeDeleted;
    foreach (const QModelIndex &index, ui->attachmentTableWidget->selectionModel()->selection().indexes()) {
      rowsToBeDeleted.append(index.row());
    }
    int prevRow = -1;
    for (int i = rowsToBeDeleted.count() - 1; i >= 0; --i) {
      int currentRow = rowsToBeDeleted.at(i);
      if (currentRow != prevRow) {
        ui->attachmentTableWidget->model()->removeRows(currentRow, 1);
        prevRow = currentRow;
      }
    }
    if (!rowsToBeDeleted.isEmpty()) {
      setDirty(true);
    }
  }
}


void MainWindow::dragEnterAttachmentWidget(QEvent *event)
{
  QDragEnterEvent *const dragEnterEvent = reinterpret_cast<QDragEnterEvent*>(event);
  if (dragEnterEvent->mimeData() != Q_NULLPTR && dragEnterEvent->mimeData()->hasUrls()) {
    foreach (const QUrl &url, dragEnterEvent->mimeData()->urls()) {
      if (url.isLocalFile()) {
        QFileInfo fi(url.toLocalFile());
        if (fi.exists() && fi.isFile() && fi.isReadable()) {
          dragEnterEvent->acceptProposedAction();
          break;
        }
      }
    }
  }
}


bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
  Q_D(MainWindow);
  // qDebug() << "MainWindow::eventFilter(" << obj->objectName() << event->type() << ")";
  switch (event->type()) {
  case QEvent::Enter:
    if (obj == ui->generatedPasswordLineEdit && !ui->generatedPasswordLineEdit->text().isEmpty()) {
      ui->generatedPasswordLineEdit->setCursor(Qt::WhatsThisCursor);
      return true;
    }
    else if (obj == ui->legacyPasswordLineEdit && !ui->legacyPasswordLineEdit->text().isEmpty()) {
      ui->legacyPasswordLineEdit->setCursor(Qt::WhatsThisCursor);
      return true;
    }
    break;
  case QEvent::Leave:
    if (obj == ui->generatedPasswordLineEdit) {
      ui->generatedPasswordLineEdit->setCursor(Qt::ArrowCursor);
      return true;
    }
    else if (obj == ui->legacyPasswordLineEdit) {
      ui->legacyPasswordLineEdit->setCursor(Qt::ArrowCursor);
      return true;
    }
    break;
  case QEvent::MouseButtonPress:
      if (obj == ui->generatedPasswordLineEdit) {
        ui->generatedPasswordLineEdit->setEchoMode(QLineEdit::Normal);
        return true;
      }
      else if (obj == ui->legacyPasswordLineEdit) {
        ui->legacyPasswordLineEdit->setEchoMode(QLineEdit::Normal);
        return true;
      }
    break;
  case QEvent::MouseButtonRelease:
      if (obj ==ui->generatedPasswordLineEdit) {
        ui->generatedPasswordLineEdit->setEchoMode(QLineEdit::Password);
        return true;
      }
      else if (obj == ui->legacyPasswordLineEdit) {
        ui->legacyPasswordLineEdit->setEchoMode(QLineEdit::Password);
        return true;
      }
    break;
  case QEvent::DragEnter:
    if (obj == ui->attachmentTableWidget) {
      dragEnterAttachmentWidget(event);
      restartInvalidationTimer();
      return true;
    }
    break;
  case QEvent::Drop:
    if (obj == ui->attachmentTableWidget) {
      QDropEvent *const dropEvent = reinterpret_cast<QDropEvent*>(event);
      if (dropEvent->mimeData() != Q_NULLPTR && dropEvent->mimeData()->hasUrls()) {
        foreach (const QUrl &url, dropEvent->mimeData()->urls()) {
          if (url.isLocalFile()) {
            attachFile(url.toLocalFile());
          }
        }
        dropEvent->accept();
        restartInvalidationTimer();
        return true;
      }
    }
    break;
  case QEvent::ContextMenu:
    if (obj == ui->attachmentTableWidget) {
      executeAttachmentContextMenu(event);
      restartInvalidationTimer();
    }
    break;
  default:
    break;
  }
  return QObject::eventFilter(obj, event);
}


void MainWindow::deleteAttachment(const QTableWidgetItem *item)
{
  Q_D(MainWindow);
  if (item != Q_NULLPTR) {
    const int row = attachmentRow(item->text());
    if (row >= 0) {
      ui->attachmentTableWidget->removeRow(row);
      setDirty(true);
    }
  }
}


void MainWindow::saveAttachmentAs(const QTableWidgetItem *item)
{
  Q_D(MainWindow);
  if (item != Q_NULLPTR) {
    QString filename = QFileDialog::getSaveFileName(this, tr("Save attachment as ..."), d->lastSaveAttachmentDir + "/" + item->text());
    if (!filename.isEmpty()) {
      QFile f(filename);
      bool ok = f.open(QIODevice::WriteOnly);
      if (ok) {
        d->lastSaveAttachmentDir = QFileInfo(filename).absolutePath();
        const QByteArray &contents = QByteArray::fromBase64(item->data(Qt::UserRole).toByteArray());
        f.write(contents);
        f.close();
      }
    }
  }
}


int MainWindow::attachmentRow(const QString &filename) const
{
  int row = -1;
  for (int i = 0; i < ui->attachmentTableWidget->rowCount(); ++i) {
    if (ui->attachmentTableWidget->item(i, 0)->text() == filename) {
      row = i;
      break;
    }
  }
  return row;
}


bool MainWindow::attachmentExists(const QString &filename) const
{
  return attachmentRow(filename) >= 0;
}


static QString toKbyte(qint64 a)
{
  const qreal sz = static_cast<qreal>(a) / 1024;
  return (sz < 1)
      ? QObject::tr("%1 B").arg(a)
      : (sz < 1024)
        ? QObject::tr("%1 KB").arg(sz, 0, 'f', 2)
        : QObject::tr("%1 MB").arg(sz / 1024, 0, 'f', 2);
}


void MainWindow::appendAttachmentToTable(const QString &filename, const QByteArray &contents)
{
  // qDebug() << "MainWindow::appendAttachmentToTable(" << filename << "," << contents << ")";
  const int row = ui->attachmentTableWidget->rowCount();
  ui->attachmentTableWidget->insertRow(row);
  QTableWidgetItem *const itemFilename = new QTableWidgetItem(filename);
  itemFilename->setData(Qt::UserRole, contents);
  itemFilename->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  ui->attachmentTableWidget->setItem(row, 0, itemFilename);
  QTableWidgetItem *const itemSize = new QTableWidgetItem(toKbyte(contents.size()));
  itemSize->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
  ui->attachmentTableWidget->setItem(row, 1, itemSize);
}


void MainWindow::setAttachments(const QVariantMap &attachments)
{
  Q_D(MainWindow);
  ui->attachmentTableWidget->setRowCount(0);
  foreach (QString key, attachments.keys()) {
    appendAttachmentToTable(key, attachments[key].toByteArray());
  }
}


void MainWindow::attachFile(const QString &filename)
{
  Q_D(MainWindow);
  bool anyAttached = false;
  QFileInfo fi(filename);
  const QString &fn = fi.fileName();
  if (!attachmentExists(fn)) {
    if (fi.size() < d->optionsDialog->maxAttachmentSizeKbyte() * 1024) {
      QFile f(filename);
      const bool ok = f.open(QIODevice::ReadOnly);
      if (ok) {
        QByteArray contents = f.readAll().toBase64();
        f.close();
        appendAttachmentToTable(fn, contents);
        anyAttached = true;
      }
      else {
        QMessageBox::information(
              this,
              tr("Read error"),
              tr("The file '%1' was not added because it cannot be read (%2).")
              .arg(fn)
              .arg(f.errorString())
              );
      }
    }
    else {
      QMessageBox::information(
            this,
            tr("Attachment too large"),
            tr("The file '%1' was not added because it's too large. "
               "Your file has %2 KByte, but only %3 KByte are allowed. "
               "You can change this limit via Extras/Options/Misc.")
            .arg(fn)
            .arg(fi.size() / 1024)
            .arg(d->optionsDialog->maxAttachmentSizeKbyte())
            );
    }
  }
  else {
    QMessageBox::information(
          this,
          tr("Attachment already exists"),
          tr("The file '%1' was not added because an attachment with the same name already exists.")
          .arg(fn));
  }
  if (anyAttached) {
    setDirty(true);
    d->lastAttachFileDir = fi.absolutePath();
  }
}


void MainWindow::onAttachFile(void)
{
  Q_D(MainWindow);
  QStringList filenames = QFileDialog::getOpenFileNames(
        this, tr("Attach files"), d->lastAttachFileDir);
  if (!filenames.isEmpty()) {
    foreach (QString filename, filenames) {
      QFileInfo fi(filename);
      if (fi.exists()) {
        attachFile(filename);
      }
    }
  }
}


QString MainWindow::defaultLocale(void)
{
  QString locale = QLocale::system().name();
  locale.truncate(locale.lastIndexOf('_'));
  return locale;
}


void MainWindow::createLanguageMenu(void)
{
  Q_D(MainWindow);
  if (d->langGroup == Q_NULLPTR) {
    d->langGroup = new QActionGroup(ui->menuBar);
    d->langGroup->setExclusive(true);
    QObject::connect(d->langGroup, SIGNAL(triggered(QAction*)), SLOT(onSelectLanguage(QAction*)));
    QDir dir(":/translations");
    const QString &setLocale = d->settings.value("mainwindow/language").toString();
    auto addLangAction = [&](const QString &locale) {
      const QString &lang = QLocale::languageToString(QLocale(locale).language());
      QAction *action = new QAction(lang, this);
      action->setCheckable(true);
      action->setData(locale);
      ui->menuLanguage->addAction(action);
      d->langGroup->addAction(action);
      if (setLocale == locale) {
        action->setChecked(true);
      }
    };
    addLangAction("en");
    const QStringList &filenames = dir.entryList(QStringList("QtSESAM_*.qm"));
    foreach (QString filename, filenames) {
      QString locale = filename;
      locale.truncate(locale.lastIndexOf('.'));
      locale.remove(0, locale.indexOf('_') + 1);
      addLangAction(locale);
    }
  }
}


void MainWindow::setLanguage(const QString &language)
{
  Q_D(MainWindow);
  d->language = language;
  d->settings.setValue("mainwindow/language", language);
  d->settings.sync();
}


void MainWindow::onSelectLanguage(QAction *action)
{
  Q_D(MainWindow);
  if (action != Q_NULLPTR) {
    const QString &newLanguage = action->data().toString();
    if (newLanguage != d->language) {
      setLanguage(newLanguage);
      QMessageBox::StandardButton button =
          QMessageBox::question(
            this,
            tr("Changed language"),
            tr("You've changed Qt-SESAM's language. "
               "Do you want to restart Qt-SESAM to take the change into effect?"));
      if (button == QMessageBox::Yes) {
        d->lockFile->unlock();
        _LOG("Restart.");
        qApp->exit(EXIT_CODE_RESTART_APP);
      }
    }
  }
}
