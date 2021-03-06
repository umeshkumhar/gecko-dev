/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_a11y_NotificationController_h_
#define mozilla_a11y_NotificationController_h_

#include "EventQueue.h"

#include "mozilla/IndexSequence.h"
#include "mozilla/Tuple.h"
#include "nsCycleCollectionParticipant.h"
#include "nsRefreshDriver.h"

#ifdef A11Y_LOG
#include "Logging.h"
#endif

namespace mozilla {
namespace a11y {

class DocAccessible;

/**
 * Notification interface.
 */
class Notification
{
public:
  NS_INLINE_DECL_REFCOUNTING(mozilla::a11y::Notification)

  /**
   * Process notification.
   */
  virtual void Process() = 0;

protected:
  Notification() { }

  /**
   * Protected destructor, to discourage deletion outside of Release():
   */
  virtual ~Notification() { }

private:
  Notification(const Notification&);
  Notification& operator = (const Notification&);
};


/**
 * Template class for generic notification.
 *
 * @note  Instance is kept as a weak ref, the caller must guarantee it exists
 *        longer than the document accessible owning the notification controller
 *        that this notification is processed by.
 */
template<class Class, class ... Args>
class TNotification : public Notification
{
public:
  typedef void (Class::*Callback)(Args* ...);

  TNotification(Class* aInstance, Callback aCallback, Args* ... aArgs) :
    mInstance(aInstance), mCallback(aCallback), mArgs(aArgs...) { }
  virtual ~TNotification() { mInstance = nullptr; }

  virtual void Process() override
    { ProcessHelper(typename IndexSequenceFor<Args...>::Type()); }

private:
  TNotification(const TNotification&);
  TNotification& operator = (const TNotification&);

  template <size_t... Indices>
    void ProcessHelper(IndexSequence<Indices...>)
  {
     (mInstance->*mCallback)(Get<Indices>(mArgs)...);
  }

  Class* mInstance;
  Callback mCallback;
  Tuple<RefPtr<Args> ...> mArgs;
};

/**
 * Used to process notifications from core for the document accessible.
 */
class NotificationController final : public EventQueue,
                                     public nsARefreshObserver
{
public:
  NotificationController(DocAccessible* aDocument, nsIPresShell* aPresShell);

  NS_IMETHOD_(MozExternalRefCountType) AddRef(void) override;
  NS_IMETHOD_(MozExternalRefCountType) Release(void) override;

  NS_DECL_CYCLE_COLLECTION_NATIVE_CLASS(NotificationController)

  /**
   * Shutdown the notification controller.
   */
  void Shutdown();

  /**
   * Put an accessible event into the queue to process it later.
   */
  void QueueEvent(AccEvent* aEvent)
  {
    if (PushEvent(aEvent))
      ScheduleProcessing();
  }

  /**
   * Schedule binding the child document to the tree of this document.
   */
  void ScheduleChildDocBinding(DocAccessible* aDocument);

  /**
   * Schedule the accessible tree update because of rendered text changes.
   */
  inline void ScheduleTextUpdate(nsIContent* aTextNode)
  {
    // Make sure we are not called with a node that is not in the DOM tree or
    // not visible.
    MOZ_ASSERT(aTextNode->GetParentNode(), "A text node is not in DOM");
    MOZ_ASSERT(aTextNode->GetPrimaryFrame(), "A text node doesn't have a frame");
    MOZ_ASSERT(aTextNode->GetPrimaryFrame()->StyleVisibility()->IsVisible(),
               "A text node is not visible");

    mTextHash.PutEntry(aTextNode);
    ScheduleProcessing();
  }

  /**
   * Pend accessible tree update for content insertion.
   */
  void ScheduleContentInsertion(Accessible* aContainer,
                                nsIContent* aStartChildNode,
                                nsIContent* aEndChildNode);

  /**
   * Pend an accessible subtree relocation.
   */
  void ScheduleRelocation(Accessible* aOwner)
  {
    if (!mRelocations.Contains(aOwner) && mRelocations.AppendElement(aOwner)) {
      ScheduleProcessing();
    }
  }

  /**
   * Start to observe refresh to make notifications and events processing after
   * layout.
   */
  void ScheduleProcessing();

  /**
   * Process the generic notification synchronously if there are no pending
   * layout changes and no notifications are pending or being processed right
   * now. Otherwise, queue it up to process asynchronously.
   *
   * @note  The caller must guarantee that the given instance still exists when
   *        the notification is processed.
   */
  template<class Class, class Arg>
  inline void HandleNotification(Class* aInstance,
                                 typename TNotification<Class, Arg>::Callback aMethod,
                                 Arg* aArg)
  {
    if (!IsUpdatePending()) {
#ifdef A11Y_LOG
      if (mozilla::a11y::logging::IsEnabled(mozilla::a11y::logging::eNotifications))
        mozilla::a11y::logging::Text("sync notification processing");
#endif
      (aInstance->*aMethod)(aArg);
      return;
    }

    RefPtr<Notification> notification =
      new TNotification<Class, Arg>(aInstance, aMethod, aArg);
    if (notification && mNotifications.AppendElement(notification))
      ScheduleProcessing();
  }

  /**
   * Schedule the generic notification to process asynchronously.
   *
   * @note  The caller must guarantee that the given instance still exists when
   *        the notification is processed.
   */
  template<class Class>
  inline void ScheduleNotification(Class* aInstance,
                                   typename TNotification<Class>::Callback aMethod)
  {
    RefPtr<Notification> notification =
      new TNotification<Class>(aInstance, aMethod);
    if (notification && mNotifications.AppendElement(notification))
      ScheduleProcessing();
  }

#ifdef DEBUG
  bool IsUpdating() const
    { return mObservingState == eRefreshProcessingForUpdate; }
#endif

protected:
  virtual ~NotificationController();

  nsCycleCollectingAutoRefCnt mRefCnt;
  NS_DECL_OWNINGTHREAD

  /**
   * Return true if the accessible tree state update is pending.
   */
  bool IsUpdatePending();

private:
  NotificationController(const NotificationController&);
  NotificationController& operator = (const NotificationController&);

  // nsARefreshObserver
  virtual void WillRefresh(mozilla::TimeStamp aTime) override;

private:
  /**
   * Indicates whether we're waiting on an event queue processing from our
   * notification controller to flush events.
   */
  enum eObservingState {
    eNotObservingRefresh,
    eRefreshObserving,
    eRefreshProcessing,
    eRefreshProcessingForUpdate
  };
  eObservingState mObservingState;

  /**
   * The presshell of the document accessible.
   */
  nsIPresShell* mPresShell;

  /**
   * Child documents that needs to be bound to the tree.
   */
  nsTArray<RefPtr<DocAccessible> > mHangingChildDocuments;

  /**
   * Pending accessible tree update notifications for content insertions.
   */
  nsClassHashtable<nsRefPtrHashKey<Accessible>,
                   nsTArray<nsCOMPtr<nsIContent>>> mContentInsertions;

  template<class T>
  class nsCOMPtrHashKey : public PLDHashEntryHdr
  {
  public:
    typedef T* KeyType;
    typedef const T* KeyTypePointer;

    explicit nsCOMPtrHashKey(const T* aKey) : mKey(const_cast<T*>(aKey)) {}
    explicit nsCOMPtrHashKey(const nsPtrHashKey<T> &aToCopy) : mKey(aToCopy.mKey) {}
    ~nsCOMPtrHashKey() { }

    KeyType GetKey() const { return mKey; }
    bool KeyEquals(KeyTypePointer aKey) const { return aKey == mKey; }

    static KeyTypePointer KeyToPointer(KeyType aKey) { return aKey; }
    static PLDHashNumber HashKey(KeyTypePointer aKey)
      { return NS_PTR_TO_INT32(aKey) >> 2; }

    enum { ALLOW_MEMMOVE = true };

   protected:
     nsCOMPtr<T> mKey;
  };

  /**
   * Pending accessible tree update notifications for rendered text changes.
   */
  nsTHashtable<nsCOMPtrHashKey<nsIContent> > mTextHash;

  /**
   * Other notifications like DOM events. Don't make this an AutoTArray; we
   * use SwapElements() on it.
   */
  nsTArray<RefPtr<Notification> > mNotifications;

  /**
   * Holds all scheduled relocations.
   */
  nsTArray<RefPtr<Accessible> > mRelocations;
};

} // namespace a11y
} // namespace mozilla

#endif // mozilla_a11y_NotificationController_h_
